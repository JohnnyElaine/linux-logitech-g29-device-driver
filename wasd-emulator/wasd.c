
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/usb/input.h>
#include <linux/input.h>
#include <linux/slab.h>

#define DRV_NAME "usb_steeringwheel_wasd"

struct wheel_evt {
    uint32_t buttons_be;
    uint16_t rot_be;
    uint8_t gas;
    uint8_t brk;
    uint8_t clt;
    uint8_t gr_x;
    uint8_t gr_y;
    uint8_t gr_z;
};

struct wheel {
    struct usb_device *udev;
    struct usb_interface *intf;
    struct input_dev *input;

    struct urb *irq_urb;
    struct wheel_evt *irq_data;
    dma_addr_t irq_dma;
    int irq_len;
    int irq_interval;
    int irq_ep;

    char phys[64];
    atomic_t opened;
};

static int drv_open(struct input_dev *dev) {
    struct wheel *w = input_get_drvdata(dev);
    if (!w) return -ENODEV;
    atomic_set(&w->opened, 1);
    int ret;
    if ((ret = usb_submit_urb(w->irq_urb, GFP_KERNEL))) {
        atomic_set(&w->opened, 0);
        return ret;
    }
    return 0;
}

static void drv_irq(struct urb *urb) {
    // called every 2ms?
    struct wheel *w = urb->context;
    if (!w || !atomic_read(&w->opened))
        return;

    const int status = urb->status;
    if (status) {
        if (status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN)
            return;
        dev_dbg(&w->intf->dev, "irq urb status %d\n", status);
        goto resubmit;
    }

    const struct wheel_evt *data = w->irq_data;
    const int rot = be16_to_cpu(data->buttons_be);
    // TODO set keys according to ratio
    input_report_key(w->input, KEY_W, data->gas <= 0x80);
    input_report_key(w->input, KEY_S, data->brk <= 0x80);
    input_report_key(w->input, KEY_A, rot <= 0x6000);
    input_report_key(w->input, KEY_D, rot >= 0xA000);
    input_sync(w->input);

resubmit:
    usb_submit_urb(w->irq_urb, GFP_ATOMIC);
}

static void drv_close(struct input_dev *dev) {
    struct wheel *w = input_get_drvdata(dev);
    if (!w) return;
    atomic_set(&w->opened, 0);
    usb_kill_urb(w->irq_urb);
}

static int drv_probe(struct usb_interface *intf, const struct usb_device_id *id) {
    struct usb_device *udev = interface_to_usbdev(intf);
    int ret;

    // Logitech G29
    //if (le16_to_cpu(udev->descriptor.idVendor) != 0x046d || le16_to_cpu(udev->descriptor.idProduct) != 0xc24f)
    //    return -ENODEV;

    struct usb_endpoint_descriptor *ep = NULL;
    const struct usb_host_interface *alts = intf->cur_altsetting;
    for (int i = 0; i < alts->desc.bNumEndpoints; i++) {
        struct usb_endpoint_descriptor *d = &alts->endpoint[i].desc;
        if (usb_endpoint_is_int_in(d)) {
            ep = d;
            break;
        }
    }
    if (!ep) return -ENODEV;

    struct wheel *w;
    if ((w = kzalloc(sizeof(*w), GFP_KERNEL)) == NULL)
        return -ENOMEM;

    w->udev = usb_get_dev(udev);
    w->intf = intf;
    atomic_set(&w->opened, 0);

    w->irq_ep = usb_endpoint_num(ep);
    w->irq_len = usb_endpoint_maxp(ep);
    w->irq_interval = ep->bInterval;

    if ((w->irq_urb = usb_alloc_urb(0, GFP_KERNEL)) == NULL) {
        ret = -ENOMEM;
        goto err_free;
    }

    if ((w->irq_data = usb_alloc_coherent(udev, w->irq_len, GFP_KERNEL, &w->irq_dma)) == NULL) {
        ret = -ENOMEM;
        goto err_free_urb;
    }

    if ((w->input = input_allocate_device()) == NULL) {
        ret = -ENOMEM;
        goto err_free_buf;
    }

    usb_make_path(udev, w->phys, sizeof(w->phys));
    strlcat(w->phys, "/input0", sizeof(w->phys));

    w->input->name = "USB Boot Mouse (example driver)";
    w->input->phys = w->phys;
    usb_to_input_id(udev, &w->input->id);
    w->input->dev.parent = &intf->dev;

    w->input->open = drv_open;
    w->input->close = drv_close;

    input_set_drvdata(w->input, w);

    usb_fill_int_urb(
        w->irq_urb,
        udev,
        usb_rcvintpipe(udev, ep->bEndpointAddress),
        w->irq_data,
        w->irq_len,
        drv_irq,
        w,
        w->irq_interval);

    w->irq_urb->transfer_dma = w->irq_dma;
    w->irq_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    usb_set_intfdata(intf, w);

    if ((ret = input_register_device(w->input)) != 0)
        goto err_clear_intfdata;

    dev_info(&intf->dev,
        "bound to %04x:%04x, int-in ep 0x%02x maxp %u interval %u\n",
        le16_to_cpu(udev->descriptor.idVendor),
        le16_to_cpu(udev->descriptor.idProduct),
        ep->bEndpointAddress,
        w->irq_len,
        w->irq_interval);

    return 0;

err_clear_intfdata:
    usb_set_intfdata(intf, NULL);
    input_free_device(w->input);
    w->input = NULL;
err_free_buf:
    usb_free_coherent(udev, w->irq_len, w->irq_data, w->irq_dma);
err_free_urb:
    usb_free_urb(w->irq_urb);
err_free:
    usb_put_dev(w->udev);
    kfree(w);
    return ret;
}

static void drv_disconnect(struct usb_interface *intf) {
    struct wheel *w = usb_get_intfdata(intf);
    usb_set_intfdata(intf, NULL);
    if (!w) return;

    if (w->input) {
        input_unregister_device(w->input);
        w->input = NULL;
    }

    usb_kill_urb(w->irq_urb);
    usb_free_coherent(w->udev, w->irq_len, w->irq_data, w->irq_dma);
    usb_free_urb(w->irq_urb);
    usb_put_dev(w->udev);
    kfree(w);

    dev_info(&intf->dev, "disconnected\n");
}

static const struct usb_device_id drv_id_table[] = {
    { USB_DEVICE_INTERFACE_NUMBER(0x046d, 0xc24f, 0) },
    { USB_INTERFACE_INFO(3, 1, 1) },
    {}
};
MODULE_DEVICE_TABLE(usb, drv_id_table);

static struct usb_driver drv = {
    .name = DRV_NAME,
    .probe = drv_probe,
    .disconnect = drv_disconnect,
    .id_table = drv_id_table,
};

module_usb_driver(drv);

MODULE_AUTHOR("Lorenz Stechauner");
MODULE_DESCRIPTION("Steering wheel WASD emulator");
MODULE_LICENSE("GPL");
