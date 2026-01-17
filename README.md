# Device Drivers
----------------------------------

This repository contains Linux **kernel modules** (``.ko``) that implement low-level USB input drivers and expose device events through the Linux **input subsystem** (evdev).

A helper tool (``usb_driver_manager.py``) is included to make it easier to:
- list USB devices and their **interfaces**
- load/unload (reload) a chosen ``.ko`` module
- unbind/bind a selected interface to a chosen driver during development

## Drivers
----------

### Mouse driver (``mouse/``)
USB HID boot-protocol mouse driver.

**Build:**
```bash
cd mouse
make
```

### Logitech G29 media driver (``g29_media_usb/``)
Logitech G29 driver that maps selected wheel inputs to media key events.

**Current mapping (Mode 0):**
- Red rotary clockwise / counter-clockwise  -> Volume Up / Volume Down
- Return ("Enter")                          -> Play/Pause
- Plus / Minus                              -> Next track / Previous track

**Build:**
```bash
cd g29_media_usb
make
```

After building, the module (``*.ko``) is typically placed under ``build/`` by the provided Makefiles.

## USB Driver Manager
---------------------

**Usage examples:**
```bash
# Search for modules in these driver directories (the tool searches recursively)
sudo python3 usb_driver_manager.py ./mouse ./g29_media_usb

# Or point directly at build/ directories
sudo python3 usb_driver_manager.py ./mouse/build ./g29_media_usb/build
```

**Workflow:**
1. Select the USB device.
2. (Optional but recommended for non-mouse devices) select the USB **interface** to bind.
3. Select the kernel module.
4. Confirm; the tool unbinds the current driver, reloads the module if needed, and binds the chosen interface.

## Testing
----------
After binding the driver, use ``evtest`` to confirm key events:
```bash
sudo evtest
```
