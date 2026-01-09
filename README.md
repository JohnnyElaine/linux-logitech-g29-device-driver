
Device Driver
============

This folder contains a minimal **out-of-tree** Linux kernel module that acts as a USB **boot-protocol mouse** driver.

It supports:
- Left and right buttons
- Scroll wheel
- Relative cursor motion (X/Y)

Important notes
---------------

- This is an **educational** example. Real USB mice are normally handled by the kernel HID stack (e.g. `usbhid` / `hid-generic`).
- This driver binds to HID **Boot Mouse** interfaces (class=HID, subclass=BOOT, protocol=MOUSE). Many mice work, but not all.
- To use it on a running system you typically must **unbind** the existing driver from that USB interface first.

Files
-----

- `usb_bootmouse.c` – kernel module (USB driver + input device)
- `Makefile` – builds against your running kernel headers

Build
-----

Install kernel headers/build deps (examples):

- Debian/Ubuntu: `sudo apt-get install build-essential linux-headers-$(uname -r)`
- Fedora: `sudo dnf install @development-tools kernel-devel-$(uname -r)`

Then build:

```bash
cd Device-Driver
make
```

Load
----

```bash
sudo insmod usb_bootmouse.ko
dmesg | tail -n 50
```

If you want to restrict binding to a specific device:

```bash
sudo insmod usb_bootmouse.ko match_vendor=0x046d match_product=0xc077
```

(Replace IDs with your mouse vendor/product from `lsusb`.)

Bind it to your mouse (unbind/bind)
---------------------------------

1) Find the USB interface path.

You can use `dmesg` when plugging the mouse in, or inspect:

```bash
ls -l /sys/bus/usb/devices/
```

Typical interface names look like `1-2:1.0` (bus-port:config.interface).

2) Unbind the existing HID driver (commonly `usbhid`) from that interface:

```bash
DEVIF="1-2:1.0"   # <- change this
echo -n "$DEVIF" | sudo tee /sys/bus/usb/drivers/usbhid/unbind
```

3) Bind this module to the interface:

```bash
echo -n "$DEVIF" | sudo tee /sys/bus/usb/drivers/usb_bootmouse/bind
```

At this point, the driver should create an input device (via `evdev`).

Test
----

List input devices and find the new one:

```bash
cat /proc/bus/input/devices
```

Or use `evtest`:

```bash
sudo apt-get install evtest   # or your distro equivalent
sudo evtest
```

You should see events:
- `BTN_LEFT`, `BTN_RIGHT`
- `REL_X`, `REL_Y`
- `REL_WHEEL`

Unload
------

```bash
sudo rmmod usb_bootmouse
```

If you want the original HID driver back, bind it again:

```bash
echo -n "$DEVIF" | sudo tee /sys/bus/usb/drivers/usbhid/bind
```
