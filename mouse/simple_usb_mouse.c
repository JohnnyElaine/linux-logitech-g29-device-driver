// SPDX-License-Identifier: GPL-2.0
/*
 * Simple USB Mouse Driver
 *
 * A minimal USB HID Boot Protocol mouse driver for learning purposes.
 * This driver can bind to any standard USB mouse that supports the
 * HID Boot Protocol.
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/input.h>

#define DRIVER_AUTHOR "Testor"
#define DRIVER_DESC "Simple USB Mouse Driver"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

/*
 * Driver context structure
 * This holds all the data we need for each connected mouse
 */
struct simple_usb_mouse {
    char name[128];                  /* Device name */
    char phys[64];                   /* Physical path */
    struct usb_device *usbdev;       /* USB device */
    struct input_dev *input_dev;     /* Input device for reporting events */
    struct urb *irq;                 /* URB for interrupt transfers */
    unsigned char *data;             /* Data buffer (8 bytes for mouse data) */
    dma_addr_t data_dma;            /* DMA address for data buffer */
};

/*
 * IRQ handler - called when mouse sends data
 *
 * Cooler Master MM710 format (8 bytes):
 * Byte 0: Button states
 *   Bit 0: Left button
 *   Bit 1: Right button
 *   Bit 2: Middle button
 *   Bit 3: Side button
 *   Bit 4: Extra button
 * Bytes 1: (unused)
 * Bytes 2-3: X movement (16-bit signed, little-endian)
 * Bytes 4-5: Y movement (16-bit signed, little-endian)
 * Byte 6: Wheel movement (8-bit signed)
 * Byte 7: (unused)
 */
static void simple_mouse_irq(struct urb *urb)
{
    struct simple_usb_mouse *mouse = urb->context;
    unsigned char *data = mouse->data;
    struct input_dev *dev = mouse->input_dev;
    int status;
    int16_t x_movement, y_movement;

    /* Check URB status */
    switch (urb->status) {
        case 0:
            /* Success - process the data */
            break;
        case -ECONNRESET:
        case -ENOENT:
        case -ESHUTDOWN:
            /* Device disconnected or URB killed - don't resubmit */
            pr_debug("simple_mouse:  URB stopped (status %d)\n", urb->status);
            return;
        default:
            /* Transient error - we'll resubmit and try again */
            pr_debug("simple_mouse: URB error (status %d)\n", urb->status);
            goto resubmit;
    }

    /* Debug: Print raw data bytes */
    /* pr_info("simple_mouse: RAW DATA: [0]=%02x [1]=%02x [2]=%02x [3]=%02x [4]=%02x [5]=%02x [6]=%02x [7]=%02x\n",
            data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]); */

    /* Report button states */
    input_report_key(dev, BTN_LEFT,   data[0] & 0x01);
    input_report_key(dev, BTN_RIGHT,  data[0] & 0x02);
    input_report_key(dev, BTN_MIDDLE, data[0] & 0x04);
    input_report_key(dev, BTN_SIDE,   data[0] & 0x08);
    input_report_key(dev, BTN_EXTRA,  data[0] & 0x10);

    /* Combine bytes for 16-bit movement (little-endian) */
    x_movement = (int16_t)(data[2] | (data[3] << 8));
    y_movement = (int16_t)(data[4] | (data[5] << 8));

    /* Report movement (relative coordinates) */
    input_report_rel(dev, REL_X, x_movement);
    input_report_rel(dev, REL_Y, y_movement);
    input_report_rel(dev, REL_WHEEL, (signed char) data[6]);

    /* Sync - tell input subsystem we're done with this event */
    input_sync(dev);

    resubmit:
    /* Resubmit URB to continue receiving data */
    status = usb_submit_urb(urb, GFP_ATOMIC);
    if (status) {
        dev_err(&mouse->usbdev->dev,
                "Failed to resubmit URB: %d\n", status);
    }
}

/*
 * Called when device is opened (e.g., when an application reads from it)
 * We start the URB here to save resources when mouse isn't being used
 */
static int simple_mouse_open(struct input_dev *dev)
{
    struct simple_usb_mouse *mouse = input_get_drvdata(dev);

    pr_info("simple_mouse: Device opened\n");

    mouse->irq->dev = mouse->usbdev;
    if (usb_submit_urb(mouse->irq, GFP_KERNEL)) {
        pr_err("simple_mouse: Failed to submit URB on open\n");
        return -EIO;
    }

    return 0;
}

/*
 * Called when device is closed
 * Stop the URB to save resources
 */
static void simple_mouse_close(struct input_dev *dev)
{
    struct simple_usb_mouse *mouse = input_get_drvdata(dev);

    pr_info("simple_mouse: Device closed\n");
    usb_kill_urb(mouse->irq);
}

/*
 * Probe function - called when a matching USB device is connected
 */
static int simple_mouse_probe(struct usb_interface *intf,
                              const struct usb_device_id *id)
{
    struct usb_device *usbdev = interface_to_usbdev(intf);
    struct usb_host_interface *interface;
    struct usb_endpoint_descriptor *endpoint;
    struct simple_usb_mouse *mouse;
    struct input_dev *input_dev;
    int pipe, maxp;
    int error = -ENOMEM;

    pr_info("simple_mouse:  Probing device %04x:%04x\n",
            le16_to_cpu(usbdev->descriptor.idVendor),
            le16_to_cpu(usbdev->descriptor. idProduct));

    interface = intf->cur_altsetting;

    /* Validate interface has exactly 1 endpoint */
    if (interface->desc.bNumEndpoints != 1) {
        pr_err("simple_mouse: Interface has %d endpoints (expected 1)\n",
               interface->desc.bNumEndpoints);
        return -ENODEV;
    }

    endpoint = &interface->endpoint[0]. desc;

    /* Ensure it's an interrupt IN endpoint */
    if (! usb_endpoint_is_int_in(endpoint)) {
        pr_err("simple_mouse: Endpoint is not interrupt IN\n");
        return -ENODEV;
    }

    /* Calculate pipe and max packet size */
    pipe = usb_rcvintpipe(usbdev, endpoint->bEndpointAddress);
    maxp = usb_maxpacket(usbdev, pipe);

    /* Allocate our context structure */
    mouse = kzalloc(sizeof(struct simple_usb_mouse), GFP_KERNEL);
    if (!mouse)
        return -ENOMEM;

    /* Allocate input device */
    input_dev = input_allocate_device();
    if (!input_dev) {
        pr_err("simple_mouse: Failed to allocate input device\n");
        goto fail_input_alloc;
    }

    /* Allocate DMA-coherent buffer for USB data */
    mouse->data = usb_alloc_coherent(usbdev, 8, GFP_KERNEL,
                                     &mouse->data_dma);
    if (!mouse->data) {
        pr_err("simple_mouse: Failed to allocate DMA buffer\n");
        goto fail_dma_alloc;
    }

    /* Allocate URB */
    mouse->irq = usb_alloc_urb(0, GFP_KERNEL);
    if (!mouse->irq) {
        pr_err("simple_mouse: Failed to allocate URB\n");
        goto fail_urb_alloc;
    }

    /* Store references */
    mouse->usbdev = usbdev;
    mouse->input_dev = input_dev;

    /* Build device name from USB descriptors */
    if (usbdev->manufacturer)
        strscpy(mouse->name, usbdev->manufacturer, sizeof(mouse->name));

    if (usbdev->product) {
        if (usbdev->manufacturer)
            strlcat(mouse->name, " ", sizeof(mouse->name));
        strlcat(mouse->name, usbdev->product, sizeof(mouse->name));
    }

    /* Fallback name if no descriptors available */
    if (! strlen(mouse->name)) {
        snprintf(mouse->name, sizeof(mouse->name),
                 "Simple USB Mouse %04x:%04x",
                 le16_to_cpu(usbdev->descriptor. idVendor),
                 le16_to_cpu(usbdev->descriptor.idProduct));
    }

    /* Build physical path */
    usb_make_path(usbdev, mouse->phys, sizeof(mouse->phys));
    strlcat(mouse->phys, "/input0", sizeof(mouse->phys));

    pr_info("simple_mouse: Device name: %s\n", mouse->name);
    pr_info("simple_mouse: Physical path: %s\n", mouse->phys);

    /* Configure input device */
    input_dev->name = mouse->name;
    input_dev->phys = mouse->phys;
    usb_to_input_id(usbdev, &input_dev->id);
    input_dev->dev.parent = &intf->dev;

    /* Set event types we can generate */
    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);

    /* Set button capabilities */
    input_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) |
    BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
    input_dev->keybit[BIT_WORD(BTN_MOUSE)] |= BIT_MASK(BTN_SIDE) |
    BIT_MASK(BTN_EXTRA);

    /* Set relative axis capabilities */
    input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y) |
    BIT_MASK(REL_WHEEL);

    /* Set driver data and callbacks */
    input_set_drvdata(input_dev, mouse);
    input_dev->open = simple_mouse_open;
    input_dev->close = simple_mouse_close;

    /* Initialize URB */
    usb_fill_int_urb(mouse->irq, usbdev, pipe, mouse->data,
                     (maxp > 8 ? 8 : maxp),
                     simple_mouse_irq, mouse, endpoint->bInterval);
    mouse->irq->transfer_dma = mouse->data_dma;
    mouse->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    /* Register input device with the kernel */
    error = input_register_device(mouse->input_dev);
    if (error) {
        pr_err("simple_mouse: Failed to register input device:  %d\n",
               error);
        goto fail_register;
    }

    /* Save our context in interface data */
    usb_set_intfdata(intf, mouse);

    pr_info("simple_mouse:  Probe successful!\n");
    return 0;

    /* Error handling - cleanup in reverse order */
    fail_register:
    usb_free_urb(mouse->irq);
    fail_urb_alloc:
    usb_free_coherent(usbdev, 8, mouse->data, mouse->data_dma);
    fail_dma_alloc:
    input_free_device(input_dev);
    fail_input_alloc:
    kfree(mouse);
    return error;
}

/*
 * Disconnect function - called when device is unplugged
 */
static void simple_mouse_disconnect(struct usb_interface *intf)
{
    struct simple_usb_mouse *mouse = usb_get_intfdata(intf);

    pr_info("simple_mouse: Device disconnected\n");

    /* Clear interface data */
    usb_set_intfdata(intf, NULL);

    if (mouse) {
        /* Stop URB */
        usb_kill_urb(mouse->irq);

        /* Unregister from input subsystem */
        input_unregister_device(mouse->input_dev);

        /* Free URB */
        usb_free_urb(mouse->irq);

        /* Free DMA buffer */
        usb_free_coherent(interface_to_usbdev(intf), 8,
                          mouse->data, mouse->data_dma);

        /* Free context structure */
        kfree(mouse);
    }
}

/*
 * Device ID table - matches ANY USB HID Boot Protocol mouse
 * This is the key to binding to any mouse!
 */
static const struct usb_device_id simple_mouse_id_table[] = {
    {
        USB_INTERFACE_INFO(
            USB_INTERFACE_CLASS_HID,       /* Class:  HID */
            USB_INTERFACE_SUBCLASS_BOOT,   /* Subclass: Boot */
            USB_INTERFACE_PROTOCOL_MOUSE   /* Protocol: Mouse */
        )
    },
    { } /* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, simple_mouse_id_table);

/*
 * USB Driver structure
 */
static struct usb_driver simple_mouse_driver = {
    .name       = "simple_usb_mouse",
    .probe      = simple_mouse_probe,
    .disconnect = simple_mouse_disconnect,
    .id_table   = simple_mouse_id_table,
};

/*
 * Module init/exit
 */
module_usb_driver(simple_mouse_driver);
