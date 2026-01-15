
Device Driver
======================

A collection of USB device drivers for Linux kernel, demonstrating how to interact with various USB HID devices.

## Current Drivers

### Mouse Driver (`mouse/`)
A USB HID mouse driver that supports 16-bit coordinate tracking for high-DPI gaming mice. Tested with Cooler Master MM710.

**Features:**
- Left, right, middle, side, and extra button support
- 16-bit X/Y movement (high-speed tracking)
- Scroll wheel support
- Binds to HID Boot Protocol Mouse interfaces

**Building:**
```bash
cd mouse/
make
```

## USB Driver Manager

The `usb_driver_manager.py` tool simplifies the process of binding USB devices to custom drivers.

**Usage:**
```bash
# Search for .ko files in current directory
sudo python3 usb_driver_manager.py

# Search in specific directories
sudo python3 usb_driver_manager.py ./mouse ./keyboard

# The tool will:
# 1. List available USB HID devices
# 2. Show available kernel modules (.ko files)
# 3. Unbind the device from its current driver
# 4. Unload existing module (if already loaded)
# 5. Load the new module
# 6. Bind the device to the new driver
```

## Future Drivers

- **Keyboard**: USB HID keyboard driver
- **Racing Wheel**: USB racing wheel driver with force feedback

## Requirements

- Linux kernel headers
- Python 3.6+
- Root/sudo access for driver loading and binding
