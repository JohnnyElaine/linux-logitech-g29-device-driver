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
#include <asm/unaligned.h>

MODULE_AUTHOR("LLP group 32");
MODULE_DESCRIPTION("Logitech G29 -> Media keys (USB driver)");
MODULE_LICENSE("GPL");

#define USB_VENDOR_ID_LOGITECH      0x046d
#define USB_DEVICE_ID_LOGITECH_G29  0xc24f
#define USB_DEVICE_ID_LOGITECH_G29_ALT 0xc260

#define G29_REPORT_LEN 12

/*
 * Button masks
 */
#define G29_BTN_PLUS        0x00800000u
#define G29_BTN_MINUS       0x01000000u
#define G29_BTN_RED_CW      0x02000000u
#define G29_BTN_RED_CCW     0x04000000u
#define G29_BTN_RETURN      0x08000000u
#define G29_BTN_R1          0x00000100u
#define G29_BTN_L1          0x00000200u

enum g29_mode {
	G29_MODE_MEDIA = 0,
};

static int mode = G29_MODE_MEDIA;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "Mapping mode (0=MEDIA)");

struct g29_state {
	u32 buttons;
	u16 rot;
	u8 gas;
	u8 brk;
	u8 clt;
	u8 grx;
	u8 gry;
	u8 grz;
};

/*
 * Mapping table entry
 */
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

	struct g29_state last;
};

/* Parsing */

static bool g29_parse_report(struct g29_state *out, const u8 *data, int len)
{
	if (len < G29_REPORT_LEN)
		return false;

	/* bytes 0..3 buttons bitfield */
	out->buttons = get_unaligned_le32(&data[0]);

	/* bytes 4..5 rotation */
	out->rot = get_unaligned_le16(&data[4]);

	out->gas = data[6];
	out->brk = data[7];
	out->clt = data[8];
	out->grx = data[9];
	out->gry = data[10];
	out->grz = data[11];

	return true;
}

/* Mapping policy */

static void g29_pulse_key(struct input_dev *input, unsigned short keycode)
{
	/* A pulse is a press+release within one report frame. */
	input_report_key(input, keycode, 1);
	input_report_key(input, keycode, 0);
}

static void g29_apply_media_mode(struct g29_dev *g29,
				const struct g29_state *prev,
				const struct g29_state *cur)
{
	u32 pressed = cur->buttons & ~prev->buttons;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(g29_media_edge_map); i++) {
		const struct g29_keymap_edge *e = &g29_media_edge_map[i];
		if (pressed & e->mask)
			g29_pulse_key(g29->input, e->keycode);
	}

	input_sync(g29->input);
}

static void g29_process_report(struct g29_dev *g29, const u8 *data, int len)
{
	struct g29_state cur;

	if (!g29_parse_report(&cur, data, len))
		return;

	switch (mode) {
	case G29_MODE_MEDIA:
	default:
		g29_apply_media_mode(g29, &g29->last, &cur);
		break;
	}

	g29->last = cur;
}

/* URB plumbing */

static void g29_urb_complete(struct urb *urb)
{
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

static int g29_input_open(struct input_dev *input)
{
	struct g29_dev *g29 = input_get_drvdata(input);

	g29->urb->dev = g29->udev;
	if (usb_submit_urb(g29->urb, GFP_KERNEL))
		return -EIO;

	return 0;
}

static void g29_input_close(struct input_dev *input)
{
	struct g29_dev *g29 = input_get_drvdata(input);

	usb_kill_urb(g29->urb);
}

/* USB driver binding */

static int g29_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alts = intf->cur_altsetting;
	struct usb_endpoint_descriptor *ep = NULL;
	struct g29_dev *g29;
	struct input_dev *input;
	int i, pipe, maxp, error;

	/* Find an interrupt IN endpoint capable of carrying the 12-byte report. */
	for (i = 0; i < alts->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *cand = &alts->endpoint[i].desc;
		if (!usb_endpoint_is_int_in(cand))
			continue;
		pipe = usb_rcvintpipe(udev, cand->bEndpointAddress);
		maxp = usb_maxpacket(udev, pipe);
		if (maxp >= G29_REPORT_LEN) {
			ep = cand;
			break;
		}
	}

	if (!ep)
		return -ENODEV;

	g29 = kzalloc(sizeof(*g29), GFP_KERNEL);
	if (!g29)
		return -ENOMEM;

	input = input_allocate_device();
	if (!input) {
		error = -ENOMEM;
		goto err_free_g29;
	}

	g29->udev = udev;
	g29->input = input;
	memset(&g29->last, 0, sizeof(g29->last));

	/* Allocate a fixed-size report buffer (12 bytes). */
	g29->buf = usb_alloc_coherent(udev, G29_REPORT_LEN, GFP_KERNEL, &g29->buf_dma);
	if (!g29->buf) {
		error = -ENOMEM;
		goto err_free_input;
	}

	g29->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!g29->urb) {
		error = -ENOMEM;
		goto err_free_buf;
	}

	/* Build a friendly input device name. */
	if (udev->manufacturer)
		strscpy(g29->name, udev->manufacturer, sizeof(g29->name));
	if (udev->product) {
		if (udev->manufacturer)
			strlcat(g29->name, " ", sizeof(g29->name));
		strlcat(g29->name, udev->product, sizeof(g29->name));
	}
	if (!strlen(g29->name))
		snprintf(g29->name, sizeof(g29->name),
			 "Logitech G29 Media %04x:%04x",
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

	input_set_drvdata(input, g29);
	input->open = g29_input_open;
	input->close = g29_input_close;

	pipe = usb_rcvintpipe(udev, ep->bEndpointAddress);
	usb_fill_int_urb(g29->urb, udev, pipe,
			 g29->buf, G29_REPORT_LEN,
			 g29_urb_complete, g29, ep->bInterval);
	g29->urb->transfer_dma = g29->buf_dma;
	g29->urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	error = input_register_device(input);
	if (error)
		goto err_free_urb;

	usb_set_intfdata(intf, g29);

	dev_info(&intf->dev,
		 "G29 media driver bound (ep=%02x interval=%u)\n",
		 ep->bEndpointAddress, ep->bInterval);

	return 0;

err_free_urb:
	usb_free_urb(g29->urb);
err_free_buf:
	usb_free_coherent(udev, G29_REPORT_LEN, g29->buf, g29->buf_dma);
err_free_input:
	input_free_device(input);
err_free_g29:
	kfree(g29);
	return error;
}

static void g29_disconnect(struct usb_interface *intf)
{
	struct g29_dev *g29 = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!g29)
		return;

	usb_kill_urb(g29->urb);
	input_unregister_device(g29->input);
	usb_free_urb(g29->urb);
	usb_free_coherent(interface_to_usbdev(intf), G29_REPORT_LEN,
			  g29->buf, g29->buf_dma);
	kfree(g29);

	dev_info(&intf->dev, "G29 media driver disconnected\n");
}

static const struct usb_device_id g29_id_table[] = {
	{ USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G29) },
	{ USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G29_ALT) },
	{ }
};
MODULE_DEVICE_TABLE(usb, g29_id_table);

static struct usb_driver g29_driver = {
	.name = "g29_media_usb",
	.probe = g29_probe,
	.disconnect = g29_disconnect,
	.id_table = g29_id_table,
};

module_usb_driver(g29_driver);
