// SPDX-License-Identifier: GPL-2.0
/*
 * Logitech G29 -> Media Keys (USB interface driver)
 *
 * Proof-of-concept Linux kernel module for low-level programming course.
 *
 * This driver:
 *   - Binds to a Logitech G29 USB interface (VID/PID match)
 *   - Receives 12-byte input reports via an interrupt-IN URB
 *   - Parses the report into a normalized state (Stage A)
 *   - Translates selected signals into media key events (Stage B)
 *
 * Stage A is designed to remain stable across different mapping policies.
 * Stage B is designed to be replaced/extended by swapping mapping tables
 * or adding per-signal handler functions.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/input.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/hid.h>

#include "g29_usb.h"

MODULE_AUTHOR("LLP group 16");
MODULE_DESCRIPTION("Logitech G29 USB driver");
MODULE_LICENSE("GPL");

enum g29_mode {
	G29_MODE_MEDIA = 0,
	G29_MODE_WASD = 1,
	G29_MODE_MOUSE = 2,
};

static int mode = G29_MODE_MEDIA;
module_param(mode, int, 0644);
MODULE_PARM_DESC(mode, "Initial mode (0=MEDIA, 1=WASD, 2=MOUSE)");

/* Steering curve exponent (100 = linear, 200 = squared, 150 = ^1.5)
 * Higher values reduce sensitivity at low steering angles.
 */
static int steer_curve = 200;
module_param(steer_curve, int, 0644);
MODULE_PARM_DESC(steer_curve, "Steering sensitivity curve (100=linear, 200=squared, default=200)");

static int steer_deadzone = 10;
module_param(steer_deadzone, int, 0644);
MODULE_PARM_DESC(steer_deadzone, "Steering deadzone radius from center (default=10)");

static int gas_curve = 100;
module_param(gas_curve, int, 0644);
MODULE_PARM_DESC(gas_curve, "Gas pedal sensitivity curve (100=linear, 200=squared, default=200)");

static int clutch_curve = 100;
module_param(clutch_curve, int, 0644);
MODULE_PARM_DESC(clutch_curve, "Clutch pedal sensitivity curve (100=linear, 200=squared, default=200)");

#define NORMALIZATION_PRECISION 1000

struct g29_keymap {
	u32 mask;
	unsigned short keycode;
};

static const struct g29_keymap media_mode_keymap[] = {
	/* Red rotary = volume */
	{ G29_BTN_RED_CW,  KEY_VOLUMEUP },
	{ G29_BTN_RED_CCW, KEY_VOLUMEDOWN },

	/* Return = play/pause */
	{ G29_BTN_RETURN,  KEY_PLAYPAUSE },

	/* Plus/Minus = next/prev */
	{ G29_BTN_R1,    KEY_NEXTSONG },
	{ G29_BTN_L1,   KEY_PREVIOUSSONG },
};

static const struct g29_keymap mouse_mode_keymap[] = {
	{ G29_BTN_X,      BTN_LEFT },
	{ G29_BTN_CIRCLE, BTN_RIGHT },
	{ G29_BTN_TRIANGLE, BTN_MIDDLE },
	{ G29_BTN_SQUARE, BTN_SIDE },
};

struct g29_dev {
	char name[128];
	char phys[64];

	struct usb_device *udev;
	struct input_dev *input;

	struct urb *urb;
	u8 *buf;
	dma_addr_t buf_dma;
	int maxp;
	int interval;
	int endpoint;

	struct timer_list steer_timer;
	struct timer_list mouse_timer;
	u32 steer_phase_ms;

	u32 phase_accumulator;
	u32 gas_phase_accumulator;
	u32 clutch_phase_accumulator;

	enum g29_mode current_mode;
	struct g29_state last;
};



static void g29_switch_mode(struct g29_dev *g29, enum g29_mode new_mode) {
	if (g29->current_mode == new_mode)
		return;

	/* Stop timers when leaving modes */
	if (g29->current_mode == G29_MODE_WASD) {
		timer_delete_sync(&g29->steer_timer);
	}
	if (g29->current_mode == G29_MODE_MOUSE) {
		timer_delete_sync(&g29->mouse_timer);
	}

	g29->current_mode = new_mode;
	g29->phase_accumulator = 0;
	g29->gas_phase_accumulator = 0;
	g29->clutch_phase_accumulator = 0;

	/* Start timers when entering modes */
	if (new_mode == G29_MODE_WASD) {
		mod_timer(&g29->steer_timer, jiffies + msecs_to_jiffies(2));
	}
	if (new_mode == G29_MODE_MOUSE) {
		mod_timer(&g29->mouse_timer, jiffies + msecs_to_jiffies(10));
	}

	dev_info(&g29->udev->dev, "Switched to mode: %s\n",
		 new_mode == G29_MODE_MEDIA ? "MEDIA" :
		 new_mode == G29_MODE_WASD ? "WASD" : "MOUSE");
}

static int calc_adjusted_steering_distance(int distance) {
	/* Apply non-linear steering curve:
	 * adjusted = (distance / MAX)^(curve/100) * MAX
	 *
	 * For curve=200 (squared): 5% input -> 0.25% output, 50% -> 25%, 100% -> 100%
	 * For curve=150 (^1.5):    5% input -> ~1.1% output, 50% -> ~35%, 100% -> 100%
	 * For curve=100 (linear):  5% input -> 5% output (no change)
	 *
	 * Using integer math: adjusted = (distance^2 / MAX) for curve=200
	 */
	if (steer_curve == 100) {
		return distance;
	}
	if (steer_curve == 200) {
		return(distance * distance) / WHEEL_MAX_DIST;
	}

	/* Generic power curve using normalized values (0-1000 range for precision)
	 * normalized = (distance * 1000) / WHEEL_MAX_DIST
	 * Apply power approximation, then scale back
	 */
	int normalized = (distance * NORMALIZATION_PRECISION) / WHEEL_MAX_DIST;
	int powered;

	if (steer_curve == 150) {
		/* Approximate ^1.5 with (x * sqrt(x)) */
		int sqrt_norm = int_sqrt(normalized * NORMALIZATION_PRECISION);
		powered = (normalized * sqrt_norm) / NORMALIZATION_PRECISION;
	} else {
		/* Fallback: squared for any other value > 100 */
		powered = (normalized * normalized) / NORMALIZATION_PRECISION;
	}

	return (powered * WHEEL_MAX_DIST) / NORMALIZATION_PRECISION;
}

static int calc_adjusted_pedal_distance(int pedal_pressure, int curve) {
	if (curve == 100) {
		return pedal_pressure;
	}
	if (curve == 200) {
		return (pedal_pressure * pedal_pressure) / G29_PEDAL_RELEASED;
	}

	int normalized = (pedal_pressure * NORMALIZATION_PRECISION) / G29_PEDAL_RELEASED;
	int powered;

	if (curve == 150) {
		int sqrt_norm = int_sqrt(normalized * NORMALIZATION_PRECISION);
		powered = (normalized * sqrt_norm) / NORMALIZATION_PRECISION;
	} else {
		powered = (normalized * normalized) / NORMALIZATION_PRECISION;
	}

	return (powered * G29_PEDAL_RELEASED) / NORMALIZATION_PRECISION;
}

static void mouse_mode_timer_fn(struct timer_list *t) {
	struct g29_dev *g29 = timer_container_of(g29, t, mouse_timer);

	const int rot = le16_to_cpu(g29->last.rot_le);
	int gas_pressure = G29_PEDAL_RELEASED - g29->last.gas;
	int clutch_pressure = G29_PEDAL_RELEASED - g29->last.clt;
	
	/* Calculate speed: positive for forward (gas), negative for backward (clutch) */
	int speed = gas_pressure - clutch_pressure;
	
	/* Apply deadzone to steering */
	int effective_rot = rot;
	if (abs(rot - WHEEL_CENTER) <= steer_deadzone) {
		effective_rot = WHEEL_CENTER;
	}
	
	/* Calculate angle from wheel rotation
	 * Map wheel rotation to angle: 
	 * - Center (32768) = 0° (straight up when forward)
	 * - Full left (0) = -90° (left)
	 * - Full right (65535) = +90° (right)
	 * We normalize to -1000 to +1000 for integer math
	 */
	int angle_normalized = ((effective_rot - WHEEL_CENTER) * 1000) / WHEEL_CENTER;
	
	/* Clamp angle to prevent overflow */
	if (angle_normalized > 1000) angle_normalized = 1000;
	if (angle_normalized < -1000) angle_normalized = -1000;
	
	/* Calculate movement components:
	 * dx = sin(angle) * speed
	 * dy = cos(angle) * speed
	 * 
	 * For small angles we approximate:
	 * sin(angle) ≈ angle (in normalized form)
	 * cos(angle) ≈ 1 - angle²/2 (in normalized form)
	 */
	int dx = (angle_normalized * speed) / 1000;
	
	/* For dy, we use cos approximation: cos ≈ 1000 - (angle²/2000) */
	int cos_approx = 1000 - ((angle_normalized * angle_normalized) / 2000);
	int dy = -(cos_approx * speed) / 1000;  /* Negative because forward is -Y */
	
	/* Scale down the movement for reasonable mouse speed */
	int scaled_dx = dx / 50;
	int scaled_dy = dy / 50;
	
	/* Report mouse movement if there's any */
	if (scaled_dx != 0 || scaled_dy != 0) {
		input_report_rel(g29->input, REL_X, scaled_dx);
		input_report_rel(g29->input, REL_Y, scaled_dy);
		input_sync(g29->input);
	}
	
	/* Reschedule timer if still in mouse mode */
	if (g29->current_mode == G29_MODE_MOUSE)
		mod_timer(&g29->mouse_timer, jiffies + msecs_to_jiffies(10));
}
static void wasd_mode_timer_fn(struct timer_list *t) {
	struct g29_dev *g29 = timer_container_of(g29, t, steer_timer);

	const int rot = le16_to_cpu(g29->last.rot_le);
	
	int effective_rot = rot;
	int distance_from_center = abs(rot - WHEEL_CENTER);
	if (distance_from_center <= steer_deadzone) {
		effective_rot = WHEEL_CENTER;
	}
	
	int distance_to_center = abs(effective_rot - WHEEL_CENTER);
	int adjusted_distance = calc_adjusted_steering_distance(distance_to_center);

	/* Phase accumulator approach:
	 * Accumulate the adjusted distance on each tick.
	 * When it exceeds the max distance, press the key and wrap.
	 * This gives us a duty cycle of (adjusted_distance / WHEEL_MAX_DIST).
	 *
	 * Examples (with curve=200):
	 *   5% steering  -> ~0.25% press rate
	 *   50% steering -> 25% press rate
	 *   100% steering -> 100% press rate (every tick)
	 */
	g29->phase_accumulator += adjusted_distance;

	bool press_key;
	if (g29->phase_accumulator >= WHEEL_MAX_DIST) {
		g29->phase_accumulator -= WHEEL_MAX_DIST;
		press_key = true;
	} else {
		press_key = false;
	}

	input_report_key(g29->input, KEY_A, press_key && (effective_rot < WHEEL_CENTER));
	input_report_key(g29->input, KEY_D, press_key && (effective_rot >= WHEEL_CENTER));

	/* Gas pedal (0xFF=up, 0x00=down) -> W key */
	int gas_pressure = 0xFF - g29->last.gas;
	int gas_adjusted = calc_adjusted_pedal_distance(gas_pressure, gas_curve);
	g29->gas_phase_accumulator += gas_adjusted;

	bool press_w = false;
	if (g29->gas_phase_accumulator >= G29_PEDAL_RELEASED) {
		g29->gas_phase_accumulator -= G29_PEDAL_RELEASED;
		press_w = true;
	}

	/* Clutch pedal (0xFF=up, 0x00=down) -> S key */
	int clutch_pressure = 0xFF - g29->last.clt;
	int clutch_adjusted = calc_adjusted_pedal_distance(clutch_pressure, clutch_curve);
	g29->clutch_phase_accumulator += clutch_adjusted;

	bool press_s = false;
	if (g29->clutch_phase_accumulator >= G29_PEDAL_RELEASED) {
		g29->clutch_phase_accumulator -= G29_PEDAL_RELEASED;
		press_s = true;
	}

	input_report_key(g29->input, KEY_W, press_w);
	input_report_key(g29->input, KEY_S, press_s);

	input_sync(g29->input);

	if (g29->current_mode == G29_MODE_WASD)
		mod_timer(&g29->steer_timer, jiffies + msecs_to_jiffies(2));
}

static void process_media_mode(struct g29_dev *g29, const struct g29_state *cur, const struct g29_state *prev) {
	const u32 pressed = le32_to_cpu(cur->buttons_le & ~prev->buttons_le);
	const u32 released = le32_to_cpu(~cur->buttons_le & prev->buttons_le);
	for (int i = 0; i < ARRAY_SIZE(media_mode_keymap); i++) {
		const struct g29_keymap *k = &media_mode_keymap[i];
		if (pressed & k->mask) {
			input_report_key(g29->input, k->keycode, 1);
		}
		if (released & k->mask) {
			input_report_key(g29->input, k->keycode, 0);
		}
	}
	input_sync(g29->input);
}

static void process_wasd_mode(struct g29_dev *g29, const struct g29_state *cur, const struct g29_state *prev) {
	/* WASD mode is handled by the timer function (g29_steer_timer_fn) */
	/* No additional processing needed here */
}

static void process_mouse_mode(struct g29_dev *g29, const struct g29_state *cur, const struct g29_state *prev) {
	const u32 pressed = le32_to_cpu(cur->buttons_le & ~prev->buttons_le);
	const u32 released = le32_to_cpu(~cur->buttons_le & prev->buttons_le);

	for (int i = 0; i < ARRAY_SIZE(mouse_mode_keymap); i++) {
		const struct g29_keymap *k = &mouse_mode_keymap[i];
		if (pressed & k->mask) {
			input_report_key(g29->input, k->keycode, 1);
		}
		if (released & k->mask) {
			input_report_key(g29->input, k->keycode, 0);
		}
	}

	if (pressed & G29_BTN_RED_CW) {
		input_report_rel(g29->input, REL_WHEEL, 1);
	}
	if (pressed & G29_BTN_RED_CCW) {
		input_report_rel(g29->input, REL_WHEEL, -1);
	}
	
	input_sync(g29->input);
}

static void g29_check_mode_switch(struct g29_dev *g29, const struct g29_state *cur, const struct g29_state *prev) {
	u32 pressed = le32_to_cpu(cur->buttons_le & ~prev->buttons_le);
	
	if (pressed & G29_BTN_SHARE) {
		g29_switch_mode(g29, G29_MODE_MEDIA);
	} else if (pressed & G29_BTN_OPTION) {
		g29_switch_mode(g29, G29_MODE_WASD);
	} else if (pressed & G29_BTN_PS3_LOGO) {
		g29_switch_mode(g29, G29_MODE_MOUSE);
	}
}

static void g29_process_report(struct g29_dev *g29, const u8 *data, unsigned int len) {
	if (len < 12) return;

	struct g29_state *cur = (void *) data;
	
	g29_check_mode_switch(g29, cur, &g29->last);
	
	switch (g29->current_mode) {
		case G29_MODE_MEDIA:
			process_media_mode(g29, cur, &g29->last);
			break;
		case G29_MODE_WASD:
			process_wasd_mode(g29, cur, &g29->last);
			break;
		case G29_MODE_MOUSE:
			process_mouse_mode(g29, cur, &g29->last);
			break;
	}

	g29->last = *cur;
}

static void g29_urb_complete(struct urb *urb) {
	struct g29_dev *g29 = urb->context;
	int ret;

	switch (urb->status) {
	case 0:
		break; /* success */
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return; /* cancelled/disconnected */
	default:
		goto resubmit; /* transient error */
	}

	g29_process_report(g29, g29->buf, urb->actual_length);

resubmit:
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		dev_err(&g29->udev->dev, "usb_submit_urb failed: %d\n", ret);
}

static int g29_input_open(struct input_dev *input) {
	struct g29_dev *g29 = input_get_drvdata(input);

	g29->urb->dev = g29->udev;
	if (usb_submit_urb(g29->urb, GFP_KERNEL))
		return -EIO;

	g29_switch_mode(g29, mode);

	return 0;
}

static void g29_input_close(struct input_dev *input) {
	struct g29_dev *g29 = input_get_drvdata(input);
	
	if (g29->current_mode == G29_MODE_WASD)
		timer_delete_sync(&g29->steer_timer);
	if (g29->current_mode == G29_MODE_MOUSE)
		timer_delete_sync(&g29->mouse_timer);
	
	usb_kill_urb(g29->urb);
}

static int g29_probe(struct usb_interface *intf, const struct usb_device_id *id) {
	struct usb_device *udev = interface_to_usbdev(intf);
	int ret;

	/* Find an interrupt IN endpoint capable of carrying the 12-byte report. */
	struct usb_endpoint_descriptor *ep = NULL;
	const struct usb_host_interface *alts = intf->cur_altsetting;
	for (int i = 0; i < alts->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *d = &alts->endpoint[i].desc;
		if (!usb_endpoint_is_int_in(d))
			continue;
		if (usb_maxpacket(udev, usb_rcvintpipe(udev, d->bEndpointAddress)) >= 12) {
			ep = d;
			break;
		}
	}
	if (!ep) return -ENODEV;

	struct g29_dev *g29;
	if ((g29 = kzalloc(sizeof(*g29), GFP_KERNEL)) == NULL) {
		return -ENOMEM;
	}

	struct input_dev *input;
	if ((input = input_allocate_device()) == NULL) {
		ret = -ENOMEM;
		goto err_free_g29;
	}

	g29->udev = udev;
	g29->input = input;

	g29->endpoint = usb_endpoint_num(ep);
	g29->maxp = usb_endpoint_maxp(ep);
	g29->interval = ep->bInterval;
	memset(&g29->last, 0, sizeof(g29->last));
	g29->current_mode = mode;  /* Initialize to module parameter */

	timer_setup(&g29->steer_timer, wasd_mode_timer_fn, 0);
	timer_setup(&g29->mouse_timer, mouse_mode_timer_fn, 0);

	if ((g29->buf = usb_alloc_coherent(udev, g29->maxp, GFP_KERNEL, &g29->buf_dma)) == NULL) {
		ret = -ENOMEM;
		goto err_free_input;
	}

	if ((g29->urb = usb_alloc_urb(0, GFP_KERNEL)) == NULL) {
		ret = -ENOMEM;
		goto err_free_buf;
	}

	if (udev->manufacturer)
		strscpy(g29->name, udev->manufacturer, sizeof(g29->name));
	if (udev->product) {
		if (udev->manufacturer)
			strlcat(g29->name, " ", sizeof(g29->name));
		strlcat(g29->name, udev->product, sizeof(g29->name));
	}
	if (!strlen(g29->name))
		snprintf(g29->name, sizeof(g29->name),
			 "Logitech G29 USB %04x:%04x",
			 le16_to_cpu(udev->descriptor.idVendor),
			 le16_to_cpu(udev->descriptor.idProduct));

	usb_make_path(udev, g29->phys, sizeof(g29->phys));
	strlcat(g29->phys, "/input0", sizeof(g29->phys));

	input->name = g29->name;
	input->phys = g29->phys;
	usb_to_input_id(udev, &input->id);
	input->dev.parent = &intf->dev;

	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_REL, input->evbit);

	/* Media mode keys */
	input_set_capability(input, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(input, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(input, EV_KEY, KEY_PLAYPAUSE);
	input_set_capability(input, EV_KEY, KEY_NEXTSONG);
	input_set_capability(input, EV_KEY, KEY_PREVIOUSSONG);

	/* WASD mode keys */
	input_set_capability(input, EV_KEY, KEY_W);
	input_set_capability(input, EV_KEY, KEY_A);
	input_set_capability(input, EV_KEY, KEY_S);
	input_set_capability(input, EV_KEY, KEY_D);

	/* Mouse mode capabilities */
	input_set_capability(input, EV_KEY, BTN_LEFT);
	input_set_capability(input, EV_KEY, BTN_RIGHT);
	input_set_capability(input, EV_KEY, BTN_MIDDLE);
	input_set_capability(input, EV_REL, REL_X);
	input_set_capability(input, EV_REL, REL_Y);
	input_set_capability(input, EV_REL, REL_WHEEL);

	input_set_drvdata(input, g29);
	input->open = g29_input_open;
	input->close = g29_input_close;

	usb_fill_int_urb(g29->urb, udev, usb_rcvintpipe(udev, ep->bEndpointAddress),
			 g29->buf, g29->maxp,
			 g29_urb_complete, g29, ep->bInterval);
	g29->urb->transfer_dma = g29->buf_dma;
	g29->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	if ((ret = input_register_device(input)) != 0) {
		goto err_free_urb;
	}

	usb_set_intfdata(intf, g29);

	dev_info(&intf->dev,
		 "G29 media driver bound (ep=%02x interval=%u)\n",
		 ep->bEndpointAddress, ep->bInterval);

	return 0;

err_free_urb:
	usb_free_urb(g29->urb);
err_free_buf:
	usb_free_coherent(udev, g29->maxp, g29->buf, g29->buf_dma);
err_free_input:
	input_free_device(input);
err_free_g29:
	kfree(g29);
	return ret;
}

static void g29_disconnect(struct usb_interface *intf) {
	struct g29_dev *g29 = usb_get_intfdata(intf);
	usb_set_intfdata(intf, NULL);
	if (!g29) return;
	usb_kill_urb(g29->urb);
	input_unregister_device(g29->input);
	usb_free_urb(g29->urb);
	usb_free_coherent(interface_to_usbdev(intf), g29->maxp, g29->buf, g29->buf_dma);
	kfree(g29);
	dev_info(&intf->dev, "G29 driver disconnected\n");
}

static const struct usb_device_id g29_id_table[] = {
	{ USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G29) },
	{ USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G29_ALT) },
	{ }
};
MODULE_DEVICE_TABLE(usb, g29_id_table);

static struct usb_driver g29_driver = {
	.name = "g29_usb",
	.id_table = g29_id_table,
	.probe = g29_probe,
	.disconnect = g29_disconnect,
};

module_usb_driver(g29_driver);
