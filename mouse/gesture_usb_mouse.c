// SPDX-License-Identifier: GPL-2.0
/*
 * Gesture USB Mouse Driver
 *
 * A USB HID Boot Protocol mouse driver with gesture recognition.
 * Detects shapes like "C" (copy) and "V" (paste) and sends keyboard events.
 * Based on simple_usb_mouse.c
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/jiffies.h>

#define DRIVER_AUTHOR "Testor"
#define DRIVER_DESC "Gesture USB Mouse Driver"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

/* Gesture detection parameters */
#define HISTORY_SIZE 100            /* Number of movement samples to track */
#define GESTURE_MIN_DISTANCE 150    /* Minimum total distance for gesture */
#define GESTURE_TIMEOUT_MS 1500     /* Max time for a gesture (ms) */
#define MOVEMENT_THRESHOLD 3        /* Ignore tiny movements (noise reduction) */

/* Module parameters for tuning */
static int gesture_enabled = 1;
module_param(gesture_enabled, int, 0644);
MODULE_PARM_DESC(gesture_enabled, "Enable gesture recognition (default: 1)");

static int gesture_min_distance = GESTURE_MIN_DISTANCE;
module_param(gesture_min_distance, int, 0644);
MODULE_PARM_DESC(gesture_min_distance, "Minimum distance for gesture detection");

static int debug_gestures = 0;
module_param(debug_gestures, int, 0644);
MODULE_PARM_DESC(debug_gestures, "Print debug info for gestures (default: 0)");

static int gesture_button = 2;
module_param(gesture_button, int, 0644);
MODULE_PARM_DESC(gesture_button, "Button for gestures (1=left, 2=middle, 3=right, default: 2)");

/*
 * Movement point for gesture tracking
 */
struct movement_point {
    int16_t x;
    int16_t y;
    unsigned long timestamp;
};

/*
 * Gesture statistics for debugging
 */
struct gesture_stats {
    unsigned long c_detected;
    unsigned long v_detected;
    unsigned long invalid;
};

/*
 * Driver context structure
 */
struct gesture_usb_mouse {
    char name[128];
    char phys[64];
    struct usb_device *usbdev;
    struct input_dev *input_dev;
    struct urb *irq;
    unsigned char *data;
    dma_addr_t data_dma;

    /* Gesture detection state */
    struct movement_point history[HISTORY_SIZE];
    int history_index;
    int history_count;
    unsigned long gesture_start_time;
    int total_distance;
    bool gesture_in_progress;
    bool gesture_button_held;

    /* Statistics */
    struct gesture_stats stats;
};

/*
 * Calculate distance between two points
 */
static int calculate_distance(int16_t x1, int16_t y1, int16_t x2, int16_t y2)
{
    int dx = x2 - x1;
    int dy = y2 - y1;
    /* Approximate distance: max(|dx|, |dy|) + min(|dx|, |dy|)/2 */
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    return (adx > ady) ? (adx + ady/2) : (ady + adx/2);
}

/*
 * Send keyboard shortcut
 */
static void send_keyboard_shortcut(struct input_dev *dev, unsigned int key)
{
    /* Press Ctrl+Key */
    input_report_key(dev, KEY_LEFTCTRL, 1);
    input_report_key(dev, key, 1);
    input_sync(dev);

    /* Release Key+Ctrl */
    input_report_key(dev, key, 0);
    input_report_key(dev, KEY_LEFTCTRL, 0);
    input_sync(dev);
}

/*
 * Detect "C" shape gesture
 * C shape: starts from right, curves left and down, then right
 * Pattern: movement goes left with downward curve, then curves back right
 */
static bool detect_c_gesture(struct gesture_usb_mouse *mouse)
{
    int i;
    int left_count = 0, right_count = 0;
    int down_count = 0, up_count = 0;
    int start_idx, end_idx;
    int16_t start_x, start_y, end_x, end_y;
    int width, height;

    if (mouse->history_count < 20)
        return false;

    /* Get start and end points */
    start_idx = (mouse->history_index - mouse->history_count + HISTORY_SIZE) % HISTORY_SIZE;
    end_idx = (mouse->history_index - 1 + HISTORY_SIZE) % HISTORY_SIZE;

    start_x = mouse->history[start_idx].x;
    start_y = mouse->history[start_idx].y;
    end_x = mouse->history[end_idx].x;
    end_y = mouse->history[end_idx].y;

    /* Analyze movement directions */
    for (i = 1; i < mouse->history_count; i++) {
        int idx = (start_idx + i) % HISTORY_SIZE;
        int prev_idx = (start_idx + i - 1 + HISTORY_SIZE) % HISTORY_SIZE;

        int16_t dx = mouse->history[idx].x - mouse->history[prev_idx].x;
        int16_t dy = mouse->history[idx].y - mouse->history[prev_idx].y;

        if (dx < -MOVEMENT_THRESHOLD) left_count++;
        if (dx > MOVEMENT_THRESHOLD) right_count++;
        if (dy < -MOVEMENT_THRESHOLD) up_count++;
        if (dy > MOVEMENT_THRESHOLD) down_count++;
    }

    width = (end_x > start_x) ? (end_x - start_x) : (start_x - end_x);
    height = (end_y > start_y) ? (end_y - start_y) : (start_y - end_y);

    /* C shape criteria:
     * - More leftward movement than rightward (opening faces left)
     * - Significant downward movement
     * - Start and end X positions are similar (closed C shape)
     * - Height is reasonable compared to width
     */
    bool is_c = (left_count * 2 > right_count * 3) &&
                (down_count > up_count) &&
                (width > 40) &&
                (height > 30) &&
                (width < height * 2);

    if (debug_gestures && is_c) {
        pr_info("gesture_mouse: C detected - L:%d R:%d D:%d U:%d W:%d H:%d\n",
                left_count, right_count, down_count, up_count, width, height);
    }

    return is_c;
}

/*
 * Detect "V" shape gesture
 * V shape: starts up, goes down-right or down-left, then up in opposite direction
 * Pattern: down-slope followed by up-slope
 */
static bool detect_v_gesture(struct gesture_usb_mouse *mouse)
{
    int i, turning_point = -1;
    int down_before = 0, up_after = 0;
    int start_idx, end_idx;
    int16_t start_y, end_y, lowest_y;
    int height_before = 0, height_after = 0;

    if (mouse->history_count < 15)
        return false;

    start_idx = (mouse->history_index - mouse->history_count + HISTORY_SIZE) % HISTORY_SIZE;
    end_idx = (mouse->history_index - 1 + HISTORY_SIZE) % HISTORY_SIZE;

    start_y = mouse->history[start_idx].y;
    end_y = mouse->history[end_idx].y;
    lowest_y = start_y;

    /* Find the turning point (lowest Y value) */
    for (i = 0; i < mouse->history_count; i++) {
        int idx = (start_idx + i) % HISTORY_SIZE;
        if (mouse->history[idx].y > lowest_y) {
            lowest_y = mouse->history[idx].y;
            turning_point = i;
        }
    }

    if (turning_point <= 0 || turning_point >= mouse->history_count - 1)
        return false;

    /* Count down movements before turning point */
    for (i = 1; i <= turning_point; i++) {
        int idx = (start_idx + i) % HISTORY_SIZE;
        int prev_idx = (start_idx + i - 1 + HISTORY_SIZE) % HISTORY_SIZE;

        int16_t dy = mouse->history[idx].y - mouse->history[prev_idx].y;
        if (dy > MOVEMENT_THRESHOLD) {
            down_before++;
            height_before += dy;
        }
    }

    /* Count up movements after turning point */
    for (i = turning_point + 1; i < mouse->history_count; i++) {
        int idx = (start_idx + i) % HISTORY_SIZE;
        int prev_idx = (start_idx + i - 1 + HISTORY_SIZE) % HISTORY_SIZE;

        int16_t dy = mouse->history[idx].y - mouse->history[prev_idx].y;
        if (dy < -MOVEMENT_THRESHOLD) {
            up_after++;
            height_after -= dy;
        }
    }

    /* V shape criteria:
     * - Significant downward movement in first half
     * - Significant upward movement in second half
     * - Both sides have reasonable height
     * - Turning point is roughly in the middle
     */
    bool is_v = (down_before > 5) &&
                (up_after > 5) &&
                (height_before > 40) &&
                (height_after > 40) &&
                (turning_point > mouse->history_count / 4) &&
                (turning_point < mouse->history_count * 3 / 4);

    if (debug_gestures && is_v) {
        pr_info("gesture_mouse: V detected - Down:%d Up:%d HB:%d HA:%d Turn:%d/%d\n",
                down_before, up_after, height_before, height_after,
                turning_point, mouse->history_count);
    }

    return is_v;
}

/*
 * Process accumulated gesture data
 */
static void process_gesture(struct gesture_usb_mouse *mouse)
{
    if (!gesture_enabled)
        return;

    if (mouse->total_distance < gesture_min_distance) {
        if (debug_gestures)
            pr_info("gesture_mouse: Gesture too short (%d < %d)\n",
                    mouse->total_distance, gesture_min_distance);
        return;
    }

    /* Try to detect gestures */
    if (detect_c_gesture(mouse)) {
        pr_info("gesture_mouse: C gesture detected - triggering COPY\n");
        send_keyboard_shortcut(mouse->input_dev, KEY_C);
        mouse->stats.c_detected++;
    } else if (detect_v_gesture(mouse)) {
        pr_info("gesture_mouse: V gesture detected - triggering PASTE\n");
        send_keyboard_shortcut(mouse->input_dev, KEY_V);
        mouse->stats.v_detected++;
    } else {
        if (debug_gestures)
            pr_info("gesture_mouse: No gesture matched (distance: %d, samples: %d)\n",
                    mouse->total_distance, mouse->history_count);
        mouse->stats.invalid++;
    }
}

/*
 * Reset gesture tracking state
 */
static void reset_gesture_tracking(struct gesture_usb_mouse *mouse)
{
    mouse->history_count = 0;
    mouse->history_index = 0;
    mouse->gesture_in_progress = false;
    mouse->total_distance = 0;
    mouse->gesture_start_time = 0;
}

/*
 * Add movement to gesture history
 */
static void add_movement(struct gesture_usb_mouse *mouse, int16_t dx, int16_t dy)
{
    struct movement_point *point;
    int distance;

    if (!mouse->gesture_in_progress)
        return;

    /* Ignore very small movements (noise) */
    if (dx < MOVEMENT_THRESHOLD && dx > -MOVEMENT_THRESHOLD &&
        dy < MOVEMENT_THRESHOLD && dy > -MOVEMENT_THRESHOLD)
        return;

    /* Check for timeout */
    if (time_after(jiffies, mouse->gesture_start_time + msecs_to_jiffies(GESTURE_TIMEOUT_MS))) {
        if (debug_gestures)
            pr_info("gesture_mouse: Gesture timeout\n");
        process_gesture(mouse);
        reset_gesture_tracking(mouse);
        return;
    }

    /* Calculate cumulative position */
    point = &mouse->history[mouse->history_index];
    if (mouse->history_count > 0) {
        int prev_idx = (mouse->history_index - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        point->x = mouse->history[prev_idx].x + dx;
        point->y = mouse->history[prev_idx].y + dy;

        distance = calculate_distance(mouse->history[prev_idx].x, mouse->history[prev_idx].y,
                                       point->x, point->y);
        mouse->total_distance += distance;
    } else {
        point->x = 0;
        point->y = 0;
    }

    point->timestamp = jiffies;

    mouse->history_index = (mouse->history_index + 1) % HISTORY_SIZE;
    if (mouse->history_count < HISTORY_SIZE)
        mouse->history_count++;
}

/*
 * IRQ handler - called when mouse sends data
 */
static void gesture_mouse_irq(struct urb *urb)
{
    struct gesture_usb_mouse *mouse = urb->context;
    unsigned char *data = mouse->data;
    struct input_dev *dev = mouse->input_dev;
    int status;
    int16_t x_movement, y_movement;
    bool gesture_button_pressed;

    /* Check URB status */
    switch (urb->status) {
        case 0:
            break;
        case -ECONNRESET:
        case -ENOENT:
        case -ESHUTDOWN:
            pr_debug("gesture_mouse: URB stopped (status %d)\n", urb->status);
            return;
        default:
            pr_debug("gesture_mouse: URB error (status %d)\n", urb->status);
            goto resubmit;
    }

    /* Extract movement data */
    x_movement = (int16_t)(data[2] | (data[3] << 8));
    y_movement = (int16_t)(data[4] | (data[5] << 8));

    /* Determine which button activates gestures */
    switch (gesture_button) {
        case 1: gesture_button_pressed = (data[0] & 0x01) != 0; break;  /* Left */
        case 2: gesture_button_pressed = (data[0] & 0x04) != 0; break;  /* Middle */
        case 3: gesture_button_pressed = (data[0] & 0x02) != 0; break;  /* Right */
        default: gesture_button_pressed = false; break;
    }

    /* Gesture tracking logic */
    if (gesture_enabled && gesture_button != 0) {
        /* Start gesture when gesture button is pressed */
        if (gesture_button_pressed && !mouse->gesture_button_held) {
            reset_gesture_tracking(mouse);
            mouse->gesture_in_progress = true;
            mouse->gesture_start_time = jiffies;
            if (debug_gestures)
                pr_info("gesture_mouse: Gesture started\n");
        }

        /* Track movements while gesture button is held */
        if (gesture_button_pressed && mouse->gesture_in_progress) {
            add_movement(mouse, x_movement, y_movement);
        }

        /* End gesture when gesture button is released */
        if (!gesture_button_pressed && mouse->gesture_button_held && mouse->gesture_in_progress) {
            if (debug_gestures)
                pr_info("gesture_mouse: Gesture ended (dist: %d, samples: %d)\n",
                        mouse->total_distance, mouse->history_count);
            process_gesture(mouse);
            reset_gesture_tracking(mouse);
        }

        mouse->gesture_button_held = gesture_button_pressed;
    }

    /* Report normal mouse events */
    input_report_key(dev, BTN_LEFT,   data[0] & 0x01);
    input_report_key(dev, BTN_RIGHT,  data[0] & 0x02);
    //input_report_key(dev, BTN_MIDDLE, data[0] & 0x04);
    input_report_key(dev, BTN_SIDE,   data[0] & 0x08);
    input_report_key(dev, BTN_EXTRA,  data[0] & 0x10);

    input_report_rel(dev, REL_X, x_movement);
    input_report_rel(dev, REL_Y, y_movement);
    input_report_rel(dev, REL_WHEEL, (signed char) data[6]);

    input_sync(dev);

resubmit:
    status = usb_submit_urb(urb, GFP_ATOMIC);
    if (status) {
        dev_err(&mouse->usbdev->dev,
                "Failed to resubmit URB: %d\n", status);
    }
}

/*
 * Device open callback
 */
static int gesture_mouse_open(struct input_dev *dev)
{
    struct gesture_usb_mouse *mouse = input_get_drvdata(dev);

    pr_info("gesture_mouse: Device opened\n");

    mouse->irq->dev = mouse->usbdev;
    if (usb_submit_urb(mouse->irq, GFP_KERNEL)) {
        pr_err("gesture_mouse: Failed to submit URB on open\n");
        return -EIO;
    }

    return 0;
}

/*
 * Device close callback
 */
static void gesture_mouse_close(struct input_dev *dev)
{
    struct gesture_usb_mouse *mouse = input_get_drvdata(dev);

    pr_info("gesture_mouse: Device closed\n");
    pr_info("gesture_mouse: Stats - C:%lu V:%lu Invalid:%lu\n",
            mouse->stats.c_detected, mouse->stats.v_detected, mouse->stats.invalid);
    usb_kill_urb(mouse->irq);
}

/*
 * Probe function
 */
static int gesture_mouse_probe(struct usb_interface *intf,
                               const struct usb_device_id *id)
{
    struct usb_device *usbdev = interface_to_usbdev(intf);
    struct usb_host_interface *interface;
    struct usb_endpoint_descriptor *endpoint;
    struct gesture_usb_mouse *mouse;
    struct input_dev *input_dev;
    int pipe, maxp;
    int error = -ENOMEM;

    pr_info("gesture_mouse: Probing device %04x:%04x\n",
            le16_to_cpu(usbdev->descriptor.idVendor),
            le16_to_cpu(usbdev->descriptor.idProduct));

    interface = intf->cur_altsetting;

    if (interface->desc.bNumEndpoints != 1) {
        pr_err("gesture_mouse: Interface has %d endpoints (expected 1)\n",
               interface->desc.bNumEndpoints);
        return -ENODEV;
    }

    endpoint = &interface->endpoint[0].desc;

    if (!usb_endpoint_is_int_in(endpoint)) {
        pr_err("gesture_mouse: Endpoint is not interrupt IN\n");
        return -ENODEV;
    }

    pipe = usb_rcvintpipe(usbdev, endpoint->bEndpointAddress);
    maxp = usb_maxpacket(usbdev, pipe);

    mouse = kzalloc(sizeof(struct gesture_usb_mouse), GFP_KERNEL);
    if (!mouse)
        return -ENOMEM;

    input_dev = input_allocate_device();
    if (!input_dev) {
        pr_err("gesture_mouse: Failed to allocate input device\n");
        goto fail_input_alloc;
    }

    mouse->data = usb_alloc_coherent(usbdev, 8, GFP_KERNEL, &mouse->data_dma);
    if (!mouse->data) {
        pr_err("gesture_mouse: Failed to allocate DMA buffer\n");
        goto fail_dma_alloc;
    }

    mouse->irq = usb_alloc_urb(0, GFP_KERNEL);
    if (!mouse->irq) {
        pr_err("gesture_mouse: Failed to allocate URB\n");
        goto fail_urb_alloc;
    }

    mouse->usbdev = usbdev;
    mouse->input_dev = input_dev;

    /* Build device name */
    if (usbdev->manufacturer)
        strscpy(mouse->name, usbdev->manufacturer, sizeof(mouse->name));

    if (usbdev->product) {
        if (usbdev->manufacturer)
            strlcat(mouse->name, " ", sizeof(mouse->name));
        strlcat(mouse->name, usbdev->product, sizeof(mouse->name));
    }

    if (!strlen(mouse->name)) {
        snprintf(mouse->name, sizeof(mouse->name),
                 "Gesture USB Mouse %04x:%04x",
                 le16_to_cpu(usbdev->descriptor.idVendor),
                 le16_to_cpu(usbdev->descriptor.idProduct));
    } else {
        strlcat(mouse->name, " [Gesture]", sizeof(mouse->name));
    }

    usb_make_path(usbdev, mouse->phys, sizeof(mouse->phys));
    strlcat(mouse->phys, "/input0", sizeof(mouse->phys));

    pr_info("gesture_mouse: Device name: %s\n", mouse->name);
    pr_info("gesture_mouse: Physical path: %s\n", mouse->phys);
    pr_info("gesture_mouse: Gesture detection: %s\n",
            gesture_enabled ? "ENABLED" : "DISABLED");

    /* Configure input device */
    input_dev->name = mouse->name;
    input_dev->phys = mouse->phys;
    usb_to_input_id(usbdev, &input_dev->id);
    input_dev->dev.parent = &intf->dev;

    /* Set event types - include KEY events for keyboard shortcuts */
    input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);

    /* Mouse buttons */
    input_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) |
        BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
    input_dev->keybit[BIT_WORD(BTN_MOUSE)] |= BIT_MASK(BTN_SIDE) |
        BIT_MASK(BTN_EXTRA);

    /* Keyboard keys for gestures */
    input_set_capability(input_dev, EV_KEY, KEY_LEFTCTRL);
    input_set_capability(input_dev, EV_KEY, KEY_C);
    input_set_capability(input_dev, EV_KEY, KEY_V);

    /* Relative axes */
    input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y) | BIT_MASK(REL_WHEEL);

    input_set_drvdata(input_dev, mouse);
    input_dev->open = gesture_mouse_open;
    input_dev->close = gesture_mouse_close;

    /* Initialize URB */
    usb_fill_int_urb(mouse->irq, usbdev, pipe, mouse->data,
                     (maxp > 8 ? 8 : maxp),
                     gesture_mouse_irq, mouse, endpoint->bInterval);
    mouse->irq->transfer_dma = mouse->data_dma;
    mouse->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    /* Initialize gesture tracking */
    reset_gesture_tracking(mouse);
    memset(&mouse->stats, 0, sizeof(mouse->stats));

    /* Register input device */
    error = input_register_device(mouse->input_dev);
    if (error) {
        pr_err("gesture_mouse: Failed to register input device: %d\n", error);
        goto fail_register;
    }

    usb_set_intfdata(intf, mouse);

    pr_info("gesture_mouse: Probe successful!\n");
    return 0;

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
 * Disconnect function
 */
static void gesture_mouse_disconnect(struct usb_interface *intf)
{
    struct gesture_usb_mouse *mouse = usb_get_intfdata(intf);

    pr_info("gesture_mouse: Device disconnected\n");

    usb_set_intfdata(intf, NULL);

    if (mouse) {
        pr_info("gesture_mouse: Final stats - C:%lu V:%lu Invalid:%lu\n",
                mouse->stats.c_detected, mouse->stats.v_detected,
                mouse->stats.invalid);

        usb_kill_urb(mouse->irq);
        input_unregister_device(mouse->input_dev);
        usb_free_urb(mouse->irq);
        usb_free_coherent(interface_to_usbdev(intf), 8,
                          mouse->data, mouse->data_dma);
        kfree(mouse);
    }
}

/*
 * Device ID table - same as simple_usb_mouse
 */
static const struct usb_device_id gesture_mouse_id_table[] = {
    {
        USB_INTERFACE_INFO(
            USB_INTERFACE_CLASS_HID,
            USB_INTERFACE_SUBCLASS_BOOT,
            USB_INTERFACE_PROTOCOL_MOUSE
        )
    },
    { }
};

MODULE_DEVICE_TABLE(usb, gesture_mouse_id_table);

/*
 * USB Driver structure
 */
static struct usb_driver gesture_mouse_driver = {
    .name       = "gesture_usb_mouse",
    .probe      = gesture_mouse_probe,
    .disconnect = gesture_mouse_disconnect,
    .id_table   = gesture_mouse_id_table,
};

module_usb_driver(gesture_mouse_driver);
