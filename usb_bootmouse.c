// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/usb/input.h>
#include <linux/input.h>
#include <linux/slab.h>

#define DRV_NAME "usb_bootmouse"

static ushort match_vendor;
module_param(match_vendor, ushort, 0444);
MODULE_PARM_DESC(match_vendor, "If non-zero, only bind to this USB vendor id (hex or dec)");

static ushort match_product;
module_param(match_product, ushort, 0444);
MODULE_PARM_DESC(match_product, "If non-zero, only bind to this USB product id (hex or dec)");

/* Runtime tuning parameters for devices with nonstandard report layouts */
static int force_offset = -1; /* -1 = auto, 0 = no report-id, 1 = report-id present */
module_param(force_offset, int, 0444);
MODULE_PARM_DESC(force_offset, "Force report-data offset (0 or 1). -1 = autodetect");

static bool swap_xy = false;
module_param(swap_xy, bool, 0444);
MODULE_PARM_DESC(swap_xy, "Swap X/Y axes (try true if horizontal/vertical are inverted)");

static int wheel_index = -1; /* -1 = autodetect (3+off) */
module_param(wheel_index, int, 0444);
MODULE_PARM_DESC(wheel_index, "Force wheel byte index in report (0-based). -1 = autodetect");

struct bootmouse {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct input_dev *input;

	struct urb *irq_urb;
	u8 *irq_data;
	dma_addr_t irq_dma;
	unsigned int irq_len;
	unsigned int irq_interval;
	unsigned int irq_ep;

	char phys[64];
	atomic_t opened;
};

static void bootmouse_irq(struct urb *urb)
{
	struct bootmouse *m = urb->context;
	int status = urb->status;
	u8 *data;
	s8 dx, dy, wheel;

	if (!m)
		return;

	if (!atomic_read(&m->opened))
		return;

	if (status) {
		/*
		 * -ENOENT/-ECONNRESET typically come from disconnect/kill/unlink.
		 * Avoid noisy logs; just stop resubmitting.
		 */
		if (status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN)
			return;

		dev_dbg(&m->intf->dev, "irq urb status %d\n", status);
		goto resubmit;
	}

	data = m->irq_data;

	/*
	 * Many HID mice use the Boot protocol layout:
	 *   [0]=buttons [1]=dx [2]=dy [3]=wheel
	 * Some devices prepend a report-id, shifting fields:
	 *   [0]=rid [1]=buttons [2]=dx [3]=dy ...
	 * Use a small heuristic to pick the correct offset so axes don't get swapped.
	 */
	{
		int off = 0;
		int sA = 0, sB = 0;
		/* score no-report-id layout: sum(|dx|+|dy|) at indexes 1,2 */
		if (m->irq_len >= 3)
			sA = abs((s8)data[1]) + abs((s8)data[2]);
		/* score report-id layout: sum at indexes 2,3 */
		if (m->irq_len >= 4)
			sB = abs((s8)data[2]) + abs((s8)data[3]);
		/* choose offset=1 if sB noticeably larger than sA */
		if (sB > sA * 2)
			off = 1;

		/* buttons */
		input_report_key(m->input, BTN_LEFT, !!(data[0 + off] & 0x01));
		input_report_key(m->input, BTN_RIGHT, !!(data[0 + off] & 0x02));

		/* motion */
		dx = (s8)data[1 + off];
		dy = (s8)data[2 + off];
		if (swap_xy) {
			s8 tmp = dx; dx = dy; dy = tmp;
		}
		input_report_rel(m->input, REL_X, dx);
		input_report_rel(m->input, REL_Y, dy);

		/* wheel if present -- allow override */
		if (wheel_index >= 0) {
			if ((unsigned)wheel_index < m->irq_len) {
				wheel = (s8)data[wheel_index];
				input_report_rel(m->input, REL_WHEEL, wheel);
			}
		} else if (m->irq_len >= 4 + off) {
			wheel = (s8)data[3 + off];
			input_report_rel(m->input, REL_WHEEL, wheel);
		}
	}

	input_sync(m->input);

resubmit:
	usb_submit_urb(m->irq_urb, GFP_ATOMIC);
}

static int bootmouse_open(struct input_dev *dev)
{
	struct bootmouse *m = input_get_drvdata(dev);
	int ret;

	if (!m)
		return -ENODEV;

	atomic_set(&m->opened, 1);

	ret = usb_submit_urb(m->irq_urb, GFP_KERNEL);
	if (ret) {
		atomic_set(&m->opened, 0);
		return ret;
	}

	return 0;
}

static void bootmouse_close(struct input_dev *dev)
{
	struct bootmouse *m = input_get_drvdata(dev);

	if (!m)
		return;

	atomic_set(&m->opened, 0);
	usb_kill_urb(m->irq_urb);
}

static int bootmouse_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alts;
	struct usb_endpoint_descriptor *epd = NULL;
	struct bootmouse *m;
	struct input_dev *input;
	int i, error;

	if (match_vendor && le16_to_cpu(udev->descriptor.idVendor) != match_vendor)
		return -ENODEV;
	if (match_product && le16_to_cpu(udev->descriptor.idProduct) != match_product)
		return -ENODEV;

	alts = intf->cur_altsetting;
	for (i = 0; i < alts->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *d = &alts->endpoint[i].desc;

		if (usb_endpoint_is_int_in(d)) {
			epd = d;
			break;
		}
	}
	if (!epd)
		return -ENODEV;

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	m->udev = usb_get_dev(udev);
	m->intf = intf;
	atomic_set(&m->opened, 0);

	m->irq_ep = usb_endpoint_num(epd);
	m->irq_len = usb_endpoint_maxp(epd);
	m->irq_interval = epd->bInterval;

	m->irq_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!m->irq_urb) {
		error = -ENOMEM;
		goto err_free;
	}

	m->irq_data = usb_alloc_coherent(udev, m->irq_len, GFP_KERNEL, &m->irq_dma);
	if (!m->irq_data) {
		error = -ENOMEM;
		goto err_free_urb;
	}

	input = input_allocate_device();
	if (!input) {
		error = -ENOMEM;
		goto err_free_buf;
	}

	m->input = input;

	usb_make_path(udev, m->phys, sizeof(m->phys));
	strlcat(m->phys, "/input0", sizeof(m->phys));

	input->name = "USB Boot Mouse (example driver)";
	input->phys = m->phys;
	usb_to_input_id(udev, &input->id);
	input->dev.parent = &intf->dev;

	input->open = bootmouse_open;
	input->close = bootmouse_close;

	__set_bit(EV_KEY, input->evbit);
	__set_bit(EV_REL, input->evbit);

	__set_bit(BTN_LEFT, input->keybit);
	__set_bit(BTN_RIGHT, input->keybit);

	__set_bit(REL_X, input->relbit);
	__set_bit(REL_Y, input->relbit);
	__set_bit(REL_WHEEL, input->relbit);

	input_set_drvdata(input, m);

	usb_fill_int_urb(
		m->irq_urb,
		udev,
		usb_rcvintpipe(udev, epd->bEndpointAddress),
		m->irq_data,
		m->irq_len,
		bootmouse_irq,
		m,
		m->irq_interval);

	m->irq_urb->transfer_dma = m->irq_dma;
	m->irq_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	usb_set_intfdata(intf, m);

	error = input_register_device(input);
	if (error)
		goto err_clear_intfdata;

	dev_info(&intf->dev,
		"bound to %04x:%04x, int-in ep 0x%02x maxp %u interval %u\n",
		le16_to_cpu(udev->descriptor.idVendor),
		le16_to_cpu(udev->descriptor.idProduct),
		epd->bEndpointAddress,
		m->irq_len,
		m->irq_interval);

	return 0;

err_clear_intfdata:
	usb_set_intfdata(intf, NULL);
	input_free_device(input);
	m->input = NULL;
err_free_buf:
	usb_free_coherent(udev, m->irq_len, m->irq_data, m->irq_dma);
err_free_urb:
	usb_free_urb(m->irq_urb);
err_free:
	usb_put_dev(m->udev);
	kfree(m);
	return error;
}

static void bootmouse_disconnect(struct usb_interface *intf)
{
	struct bootmouse *m = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!m)
		return;

	if (m->input) {
		/* Triggers ->close if opened */
		input_unregister_device(m->input);
		m->input = NULL;
	}

	usb_kill_urb(m->irq_urb);
	usb_free_coherent(m->udev, m->irq_len, m->irq_data, m->irq_dma);
	usb_free_urb(m->irq_urb);
	usb_put_dev(m->udev);
	kfree(m);

	dev_info(&intf->dev, "disconnected\n");
}

/* Match HID Boot Mouse interfaces. */
static const struct usb_device_id bootmouse_id_table[] = {
	/* HID Boot Mouse: bInterfaceClass = 3 (HID), bInterfaceSubClass = 1 (BOOT), bInterfaceProtocol = 2 (MOUSE) */
	{ USB_INTERFACE_INFO(3, 1, 2) },
	{ }
};
MODULE_DEVICE_TABLE(usb, bootmouse_id_table);

static struct usb_driver bootmouse_driver = {
	.name = DRV_NAME,
	.probe = bootmouse_probe,
	.disconnect = bootmouse_disconnect,
	.id_table = bootmouse_id_table,
};

module_usb_driver(bootmouse_driver);

MODULE_AUTHOR("Example / educational");
MODULE_DESCRIPTION("Minimal USB HID boot-protocol mouse driver (buttons, motion, wheel)");
MODULE_LICENSE("GPL");
