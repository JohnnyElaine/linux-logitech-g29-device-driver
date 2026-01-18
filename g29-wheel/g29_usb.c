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
};

static int mode = G29_MODE_MEDIA;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "Mapping mode (0=MEDIA)");

struct g29_keymap_edge {
	u32 mask;
	unsigned short keycode;
};

static const struct g29_keymap_edge g29_media_edge_map[] = {
	/* Red rotary = volume */
	{ G29_BTN_RED_CW,  KEY_VOLUMEUP },
	{ G29_BTN_RED_CCW, KEY_VOLUMEDOWN },

	/* Return = play/pause */
	{ G29_BTN_RETURN,  KEY_PLAYPAUSE },

	/* Plus/Minus = next/prev */
	{ G29_BTN_R1,    KEY_NEXTSONG },
	{ G29_BTN_L1,   KEY_PREVIOUSSONG },
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
	u32 steer_phase_ms;

	u32 phase_accumulator;  /* Phase accumulator for PWM-like key pressing */

	struct g29_state last;
};



static void g29_steer_timer_fn(struct timer_list *t) {
	struct g29_dev *g29 = timer_container_of(g29, t, steer_timer);

	const int rot = le16_to_cpu(g29->last.rot_le);
	int distance_from_center = abs(rot - WHEEL_CENTER);
	bool press_key;

	/* Phase accumulator approach:
	 * Accumulate the distance on each tick.
	 * When it exceeds the max distance, press the key and wrap.
	 * This gives us a duty cycle of (distance / WHEEL_MAX_DIST).
	 *
	 * Examples:
	 *   distance = WHEEL_MAX_DIST/2 (50%) -> press every 2nd tick
	 *   distance = WHEEL_MAX_DIST (100%)   -> press every tick
	 *   distance = WHEEL_MAX_DIST/4 (25%)  -> press every 4th tick
	 */
	g29->phase_accumulator += distance_from_center;

	if (g29->phase_accumulator >= WHEEL_MAX_DIST) {
		g29->phase_accumulator -= WHEEL_MAX_DIST;
		press_key = true;
	} else {
		press_key = false;
	}

	/* Report the appropriate horizontal key */
	input_report_key(g29->input, KEY_A, press_key && (rot < WHEEL_CENTER));
	input_report_key(g29->input, KEY_D, press_key && (rot >= WHEEL_CENTER));

	/* Gas/clutch to W/S (note: 0xFF is unpressed, 0x00 is fully pressed) */
	input_report_key(g29->input, KEY_W, g29->last.gas <= 0x80);
	input_report_key(g29->input, KEY_S, g29->last.clt <= 0x80);

	input_sync(g29->input);

	mod_timer(&g29->steer_timer, jiffies + msecs_to_jiffies(2));
}

static void g29_apply_media_mode(struct g29_dev *g29, const struct g29_state *cur, const struct g29_state *prev) {
	u32 pressed = le32_to_cpu(cur->buttons_le & ~prev->buttons_le);
	for (int i = 0; i < ARRAY_SIZE(g29_media_edge_map); i++) {
		const struct g29_keymap_edge *e = &g29_media_edge_map[i];
		if (pressed & e->mask) {
			input_report_key(g29->input, e->keycode, 1);
			input_report_key(g29->input, e->keycode, 0);
		}
	}

	input_sync(g29->input);
}

static void g29_process_report(struct g29_dev *g29, const u8 *data, unsigned int len) {
	if (len < 12) return;

	struct g29_state *cur = (void *) data;
	switch (mode) {
		case G29_MODE_MEDIA:
		default:
			g29_apply_media_mode(g29, cur, &g29->last);
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

	mod_timer(&g29->steer_timer, jiffies + msecs_to_jiffies(2));

	return 0;
}

static void g29_input_close(struct input_dev *input) {
	struct g29_dev *g29 = input_get_drvdata(input);
	timer_delete_sync(&g29->steer_timer);
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

	timer_setup(&g29->steer_timer, g29_steer_timer_fn, 0);

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

	/* Advertise only the keys we emit in media mode. */
	input_set_capability(input, EV_KEY, KEY_VOLUMEUP);
	input_set_capability(input, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(input, EV_KEY, KEY_PLAYPAUSE);
	input_set_capability(input, EV_KEY, KEY_NEXTSONG);
	input_set_capability(input, EV_KEY, KEY_PREVIOUSSONG);

	input_set_capability(input, EV_KEY, KEY_W);
	input_set_capability(input, EV_KEY, KEY_A);
	input_set_capability(input, EV_KEY, KEY_S);
	input_set_capability(input, EV_KEY, KEY_D);

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
