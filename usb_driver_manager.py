#!/usr/bin/env python3
"""
USB Driver Manager - CLI tool for managing USB device driver bindings
Helps with finding USB devices, unloading current drivers, loading new drivers,
and binding USB devices to new drivers.

Note: USB drivers typically bind to interfaces, not devices. This script
handles both device-level and interface-level driver binding.

Wheel-friendly improvements:
  - Finds kernel modules (*.ko) recursively under provided directories (e.g. ./build).
  - Lets the user select which USB interface(s) to unbind/bind.
    Default selection: all HID interfaces (bInterfaceClass==03).
"""

import os
import sys
import subprocess
import glob
import re
import time
import argparse
from pathlib import Path


class Colors:
    """ANSI color codes for terminal output"""
    HEADER = '\033[95m'
    BLUE = '\033[94m'
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    END = '\033[0m'
    BOLD = '\033[1m'


def print_header(text):
    """Print colored header"""
    print(f"\n{Colors.BOLD}{Colors.HEADER}{text}{Colors.END}")


def print_success(text):
    """Print success message"""
    print(f"{Colors.GREEN}✓ {text}{Colors.END}")


def print_error(text):
    """Print error message"""
    print(f"{Colors.RED}✗ {text}{Colors.END}")


def print_warning(text):
    """Print warning message"""
    print(f"{Colors.YELLOW}⚠ {text}{Colors.END}")


def check_root():
    """Check if script is running with root privileges"""
    if os.geteuid() != 0:
        print_error("This tool requires root privileges.")
        print("Please run with sudo:")
        print(f"  sudo {' '.join(sys.argv)}")
        sys.exit(1)


def get_usb_devices():
    """Get list of USB devices with their information"""
    devices = []
    usb_devices_path = Path("/sys/bus/usb/devices")

    if not usb_devices_path.exists():
        print_error("USB devices path not found. Is USB subsystem available?")
        return devices

    # Get lsusb output for human-readable names
    lsusb_output = {}
    try:
        result = subprocess.run(['lsusb'], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            # Format: Bus 001 Device 005: ID 046d:c52b Logitech, Inc. Unifying Receiver
            match = re.match(r'Bus (\d+) Device (\d+): ID ([0-9a-f]{4}):([0-9a-f]{4})\s+(.*)', line, re.IGNORECASE)
            if match:
                bus, dev, vendor, product, name = match.groups()
                key = f"{int(bus)}-{int(dev)}"
                lsusb_output[key] = {
                    'vendor_id': vendor,
                    'product_id': product,
                    'name': name.strip()
                }
    except Exception as e:
        print_warning(f"Could not run lsusb: {e}")

    # Iterate through USB devices
    for device_path in usb_devices_path.iterdir():
        if not device_path.is_dir():
            continue

        # Only process actual device entries (format: busnum-devnum or busnum-port.port...)
        device_name = device_path.name
        if not re.match(r'\d+-[\d.]+$', device_name):
            continue

        # Skip root hubs
        devpath_file = device_path / "devpath"
        if not devpath_file.exists():
            continue

        try:
            # Read device information
            vendor_id = (device_path / "idVendor").read_text().strip() if (device_path / "idVendor").exists() else "unknown"
            product_id = (device_path / "idProduct").read_text().strip() if (device_path / "idProduct").exists() else "unknown"
            manufacturer = (device_path / "manufacturer").read_text().strip() if (device_path / "manufacturer").exists() else "Unknown"
            product = (device_path / "product").read_text().strip() if (device_path / "product").exists() else "Unknown"
            busnum = (device_path / "busnum").read_text().strip() if (device_path / "busnum").exists() else "?"
            devnum = (device_path / "devnum").read_text().strip() if (device_path / "devnum").exists() else "?"

            # Get current driver
            driver = "none"
            driver_link = device_path / "driver"
            if driver_link.exists() and driver_link.is_symlink():
                driver = driver_link.resolve().name

            # Check interfaces for HID class and collect interface information
            interfaces = []
            is_input = False
            for interface_path in device_path.glob("*:*.*"):
                if not interface_path.is_dir():
                    continue
                try:
                    iface_class = (interface_path / "bInterfaceClass").read_text().strip() if (interface_path / "bInterfaceClass").exists() else "00"
                    iface_subclass = (interface_path / "bInterfaceSubClass").read_text().strip() if (interface_path / "bInterfaceSubClass").exists() else "00"
                    iface_protocol = (interface_path / "bInterfaceProtocol").read_text().strip() if (interface_path / "bInterfaceProtocol").exists() else "00"

                    # Get interface driver
                    iface_driver = "none"
                    iface_driver_link = interface_path / "driver"
                    if iface_driver_link.exists() and iface_driver_link.is_symlink():
                        iface_driver = iface_driver_link.resolve().name

                    interfaces.append({
                        'name': interface_path.name,
                        'path': str(interface_path),
                        'class': iface_class,
                        'subclass': iface_subclass,
                        'protocol': iface_protocol,
                        'driver': iface_driver
                    })

                    # Class 03 is HID (Human Interface Device)
                    if iface_class == "03":
                        is_input = True
                except Exception:
                    continue

            # Improve product string from lsusb when possible (safe conversion)
            if busnum.isdigit() and devnum.isdigit():
                lsusb_key = f"{int(busnum)}-{int(devnum)}"
                if lsusb_key in lsusb_output:
                    product = lsusb_output[lsusb_key]['name']

            devices.append({
                'path': str(device_path),
                'name': device_name,
                'vendor_id': vendor_id,
                'product_id': product_id,
                'manufacturer': manufacturer,
                'product': product,
                'bus': busnum,
                'device': devnum,
                'driver': driver,
                'is_input': is_input,
                'interfaces': interfaces,
                'display_name': f"{manufacturer} {product}".strip()
            })

        except Exception as e:
            # Skip devices that can't be read
            continue

    return devices


def display_usb_devices(devices, filter_input=True):
    """Display USB devices in a formatted list"""
    if filter_input:
        devices = [d for d in devices if d['is_input']]

    if not devices:
        print_warning("No USB devices found.")
        return None

    print_header("Available USB Devices:")
    print(f"\n{'#':<4} {'Device':<15} {'Vendor:Product':<15} {'Name':<40} {'Interfaces':<15}")
    print("-" * 100)

    for idx, device in enumerate(devices, 1):
        vendor_product = f"{device['vendor_id']}:{device['product_id']}"

        display_name = device['display_name']
        if len(display_name) > 40:
            display_name = display_name[:37] + "..."

        iface_info = f"{len(device.get('interfaces', []))} interface(s)"

        print(f"{idx:<4} {device['name']:<15} {vendor_product:<15} {display_name:<40} {iface_info}")

    return devices


def get_kernel_modules(directories=None):
    """Get list of available kernel modules (.ko files)"""
    if directories is None:
        directories = ["."]

    modules = []
    seen_modules = set()  # Track module names to avoid duplicates

    # Search for .ko files recursively in each specified directory
    for directory in directories:
        if not os.path.exists(directory):
            print_warning(f"Directory not found: {directory}")
            continue

        for root, dirs, files in os.walk(directory):
            for filename in files:
                if not filename.endswith('.ko'):
                    continue

                module_name = filename
                module_path = os.path.abspath(os.path.join(root, filename))

                # Skip duplicates (same module name already found)
                if module_name in seen_modules:
                    continue
                seen_modules.add(module_name)

                # Get module info if possible
                try:
                    result = subprocess.run(['modinfo', module_path], capture_output=True, text=True)
                    description = "No description"
                    for line in result.stdout.splitlines():
                        if line.startswith("description:"):
                            description = line.split(":", 1)[1].strip()
                            break

                    modules.append({
                        'name': module_name,
                        'path': module_path,
                        'description': description
                    })
                except Exception:
                    modules.append({
                        'name': module_name,
                        'path': module_path,
                        'description': "No description available"
                    })

    return modules


def display_kernel_modules(modules):
    """Display available kernel modules"""
    if not modules:
        print_warning("No kernel modules (.ko files) found in current directory.")
        return None

    print_header("Available Kernel Modules:")
    print(f"\n{'#':<4} {'Module Name':<30} {'Description':<50}")
    print("-" * 90)

    for idx, module in enumerate(modules, 1):
        desc = module['description']
        if len(desc) > 50:
            desc = desc[:47] + "..."
        print(f"{idx:<4} {module['name']:<30} {desc}")

    return modules


def get_user_choice(prompt, max_choice):
    """Get user input for selection"""
    while True:
        try:
            choice = input(f"\n{prompt} (1-{max_choice}, or 'q' to quit): ").strip()
            if choice.lower() == 'q':
                return None
            choice = int(choice)
            if 1 <= choice <= max_choice:
                return choice - 1  # Return 0-indexed
            else:
                print_error(f"Please enter a number between 1 and {max_choice}")
        except ValueError:
            print_error("Invalid input. Please enter a number.")
        except KeyboardInterrupt:
            print("\n")
            return None


def get_interface_selection(device):
    """Select which interface(s) to unbind/bind.

    Default (Enter): all HID interfaces (class 03).
    'a' selects all interfaces.
    Comma-separated list selects specific interfaces.
    """
    if not device.get('interfaces'):
        return []

    print_header("Interfaces")
    for idx, iface in enumerate(device['interfaces'], 1):
        driver_color = Colors.GREEN if iface['driver'] != "none" else Colors.YELLOW
        print(f"  {idx:>2}. {iface['name']}: class={iface['class']} subclass={iface['subclass']} protocol={iface['protocol']} driver={driver_color}{iface['driver']}{Colors.END}")

    print("\nSelect interfaces to bind:")
    print("  [Enter]  AUTO: all HID interfaces (class 03)")
    print("  a        ALL interfaces")
    print("  1,3      Comma-separated list")

    raw = input("Selection: ").strip().lower()
    if raw == "":
        picked = [i for i in device['interfaces'] if i['class'] == "03"]
        if not picked:
            print_warning("No HID interfaces found; selecting ALL interfaces.")
            picked = list(device['interfaces'])
        return picked

    if raw == "a":
        return list(device['interfaces'])

    picked = []
    parts = [p.strip() for p in raw.split(',') if p.strip()]
    for p in parts:
        try:
            i = int(p)
            if 1 <= i <= len(device['interfaces']):
                picked.append(device['interfaces'][i - 1])
        except ValueError:
            continue

    if not picked:
        print_warning("No valid interfaces selected; using AUTO (all HID interfaces).")
        picked = [i for i in device['interfaces'] if i['class'] == "03"]
        if not picked:
            picked = list(device['interfaces'])
    return picked


def unbind_device(device, interface=None):
    """Unbind device interface from current driver"""
    # If specific interface provided, unbind that interface
    if interface:
        if interface['driver'] == "none":
            print_warning(f"Interface {interface['name']} is not bound to any driver.")
            return True

        driver_path = Path(f"/sys/bus/usb/drivers/{interface['driver']}")
        unbind_path = driver_path / "unbind"

        if not unbind_path.exists():
            print_error(f"Cannot unbind: {unbind_path} not found")
            return False

        try:
            print(f"Unbinding interface {interface['name']} from driver {interface['driver']}...")
            unbind_path.write_text(interface['name'])
            print_success(f"Successfully unbound from {interface['driver']}")
            return True
        except Exception as e:
            print_error(f"Failed to unbind interface: {e}")
            return False

    # Otherwise unbind all interfaces
    success = True
    for iface in device.get('interfaces', []):
        if iface['driver'] != "none":
            if not unbind_device(device, iface):
                success = False

    if not device.get('interfaces'):
        # Fallback to old behavior for device-level driver
        if device['driver'] == "none":
            print_warning("Device is not bound to any driver.")
            return True

        driver_path = Path(f"/sys/bus/usb/drivers/{device['driver']}")
        unbind_path = driver_path / "unbind"

        if not unbind_path.exists():
            print_error(f"Cannot unbind: {unbind_path} not found")
            return False

        try:
            print(f"Unbinding device {device['name']} from driver {device['driver']}...")
            unbind_path.write_text(device['name'])
            print_success(f"Successfully unbound from {device['driver']}")
            return True
        except Exception as e:
            print_error(f"Failed to unbind device: {e}")
            return False

    return success


def load_module(module):
    """Load kernel module"""
    try:
        print(f"Loading module {module['name']}...")
        result = subprocess.run(['insmod', module['path']], capture_output=True, text=True)
        if result.returncode == 0:
            print_success(f"Successfully loaded {module['name']}")
            return True
        else:
            # Module might already be loaded
            if "File exists" in result.stderr or "already" in result.stderr.lower():
                print_warning(f"Module {module['name']} is already loaded")
                return True
            print_error(f"Failed to load module: {result.stderr}")
            return False
    except Exception as e:
        print_error(f"Failed to load module: {e}")
        return False


def get_module_driver_name(module):
    """Get the driver name from module"""
    # Extract driver name from module (remove .ko extension)
    driver_name = os.path.splitext(module['name'])[0]

    # Check if driver exists in /sys/bus/usb/drivers/
    driver_path = Path(f"/sys/bus/usb/drivers/{driver_name}")
    if driver_path.exists():
        return driver_name

    # Try to get it from loaded modules
    try:
        result = subprocess.run(['lsmod'], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            if line.startswith(driver_name):
                return driver_name
    except Exception:
        pass

    return driver_name


def bind_device(device, module, interface=None):
    """Bind device interface to new driver"""
    driver_name = get_module_driver_name(module)
    driver_path = Path(f"/sys/bus/usb/drivers/{driver_name}")
    bind_path = driver_path / "bind"

    if not driver_path.exists():
        print_error(f"Driver path not found: {driver_path}")
        print_warning("The driver might not be loaded or might use a different name.")
        return False

    if not bind_path.exists():
        print_error(f"Bind interface not found: {bind_path}")
        return False

    # If specific interface provided, bind that interface
    if interface:
        # Check if already bound to target driver
        iface_path = Path(interface['path'])
        driver_link = iface_path / "driver"
        if driver_link.exists() and driver_link.is_symlink():
            current_driver = driver_link.resolve().name
            if current_driver == driver_name:
                print_success(f"Interface {interface['name']} already bound to {driver_name}")
                return True

        try:
            print(f"Binding interface {interface['name']} to driver {driver_name}...")
            bind_path.write_text(interface['name'])
            print_success(f"Successfully bound to {driver_name}")
            return True
        except Exception as e:
            # Check again if it got bound (might be EBUSY because it auto-bound)
            if driver_link.exists() and driver_link.is_symlink():
                current_driver = driver_link.resolve().name
                if current_driver == driver_name:
                    print_success(f"Interface {interface['name']} bound to {driver_name} (auto-probed)")
                    return True
            print_error(f"Failed to bind interface: {e}")
            return False

    # Default behavior (kept for backwards compatibility): try boot mouse, then device.
    target_interfaces = []
    for iface in device.get('interfaces', []):
        # Look for HID Boot Mouse interfaces
        if iface['class'] == "03" and iface['subclass'] == "01" and iface['protocol'] == "02":
            target_interfaces.append(iface)

    if not target_interfaces:
        # Fallback: try to bind the device itself
        try:
            print(f"Binding device {device['name']} to driver {driver_name}...")
            bind_path.write_text(device['name'])
            print_success(f"Successfully bound to {driver_name}")
            return True
        except Exception as e:
            print_error(f"Failed to bind device: {e}")
            return False

    # Bind each target interface
    success = True
    for iface in target_interfaces:
        # Check if already bound to target driver
        iface_path = Path(iface['path'])
        driver_link = iface_path / "driver"
        if driver_link.exists() and driver_link.is_symlink():
            current_driver = driver_link.resolve().name
            if current_driver == driver_name:
                print_success(f"Interface {iface['name']} already bound to {driver_name}")
                continue

        try:
            print(f"Binding interface {iface['name']} to driver {driver_name}...")
            bind_path.write_text(iface['name'])
            print_success(f"Successfully bound interface {iface['name']} to {driver_name}")
        except Exception as e:
            # Check again if it got bound (might be EBUSY because it auto-bound)
            if driver_link.exists() and driver_link.is_symlink():
                current_driver = driver_link.resolve().name
                if current_driver == driver_name:
                    print_success(f"Interface {iface['name']} bound to {driver_name} (auto-probed)")
                    continue
            print_error(f"Failed to bind interface {iface['name']}: {e}")
            success = False

    return success


def bind_interface_to_driver(interface_name, driver_name):
    """Bind a specific USB interface back to a given driver name (sysfs)."""
    driver_path = Path(f"/sys/bus/usb/drivers/{driver_name}")
    bind_path = driver_path / "bind"

    if not driver_path.exists():
        print_warning(f"Cannot restore: driver path not found: {driver_path}")
        return False

    if not bind_path.exists():
        print_warning(f"Cannot restore: bind path not found: {bind_path}")
        return False

    try:
        print(f"Restoring interface {interface_name} to driver {driver_name}...")
        bind_path.write_text(interface_name)
        print_success(f"Restored {interface_name} to {driver_name}")
        return True
    except Exception as e:
        print_warning(f"Failed to restore interface {interface_name} to {driver_name}: {e}")
        return False


def unload_module(module):
    """Unload kernel module"""
    driver_name = get_module_driver_name(module)
    try:
        print(f"Unloading module {driver_name}...")
        result = subprocess.run(['rmmod', driver_name], capture_output=True, text=True)
        if result.returncode == 0:
            print_success(f"Successfully unloaded {driver_name}")
            return True
        else:
            print_warning(f"Could not unload module: {result.stderr}")
            return False
    except Exception as e:
        print_warning(f"Could not unload module: {e}")
        return False


def is_module_loaded(module):
    """Check if a kernel module is currently loaded"""
    driver_name = get_module_driver_name(module)
    try:
        result = subprocess.run(['lsmod'], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            if line.split()[0] == driver_name:
                return True
        return False
    except Exception:
        return False


def main():
    """Main function"""
    # Parse command line arguments
    parser = argparse.ArgumentParser(
        description='USB Driver Manager - CLI tool for managing USB device driver bindings',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Examples:\n'
               '  sudo %(prog)s                          # Search for .ko files in current directory\n'
               '  sudo %(prog)s /path/to/modules         # Search in specific directory\n'
               '  sudo %(prog)s . /path/to/dir2          # Search in multiple directories\n'
    )
    parser.add_argument(
        'directories',
        nargs='*',
        default=['.'],
        help='Directories to search for kernel modules (.ko files). Defaults to current directory.'
    )
    args = parser.parse_args()

    print(f"{Colors.BOLD}{Colors.CYAN}")
    print("=" * 60)
    print("       USB Driver Manager")
    print("=" * 60)
    print(Colors.END)

    # Check root privileges
    check_root()

    # Step 1: List USB devices
    devices = get_usb_devices()
    displayed_devices = display_usb_devices(devices, filter_input=True)

    if not displayed_devices:
        print_error("No input devices found. Showing all USB devices...")
        displayed_devices = display_usb_devices(devices, filter_input=False)
        if not displayed_devices:
            sys.exit(1)

    # Step 2: Select device
    device_idx = get_user_choice("Select USB device", len(displayed_devices))
    if device_idx is None:
        print("\nOperation cancelled.")
        sys.exit(0)

    selected_device = displayed_devices[device_idx]
    print(f"\n{Colors.CYAN}Selected device: {selected_device['display_name']}{Colors.END}")
    print(f"  Device: {selected_device['name']}")
    print(f"  Current driver: {selected_device['driver']}")

    # Step 2b: Select interfaces
    selected_interfaces = get_interface_selection(selected_device)
    if not selected_interfaces:
        print_warning("No interfaces available/selected; cannot bind.")
        sys.exit(1)

    # Remember original per-interface drivers for restore attempts
    original_interface_drivers = {}
    for iface in selected_interfaces:
        original_interface_drivers[iface['name']] = iface.get('driver', 'none')

    # Step 3: List available kernel modules
    if len(args.directories) > 1 or args.directories[0] != '.':
        print(f"\nSearching for kernel modules in: {', '.join(args.directories)}")
    modules = get_kernel_modules(args.directories)
    displayed_modules = display_kernel_modules(modules)

    if not displayed_modules:
        sys.exit(1)

    # Step 4: Select module
    module_idx = get_user_choice("Select kernel module", len(displayed_modules))
    if module_idx is None:
        print("\nOperation cancelled.")
        sys.exit(0)

    selected_module = displayed_modules[module_idx]
    print(f"\n{Colors.CYAN}Selected module: {selected_module['name']}{Colors.END}")

    # Check if module is already loaded
    module_already_loaded = is_module_loaded(selected_module)
    if module_already_loaded:
        print_warning(f"Module {selected_module['name']} is already loaded and will be reloaded.")

    # Step 5: Confirm operation
    print(f"\n{Colors.YELLOW}This will:{Colors.END}")
    print(f"  1. Unbind {len(selected_interfaces)} interface(s) from current driver(s)")
    if module_already_loaded:
        print(f"  2. Unload existing module {selected_module['name']}")
        print(f"  3. Load module {selected_module['name']} (fresh version)")
        print(f"  4. Bind selected interface(s) to the new driver")
    else:
        print(f"  2. Load module {selected_module['name']}")
        print(f"  3. Bind selected interface(s) to the new driver")

    confirm = input(f"\n{Colors.BOLD}Proceed? (yes/no): {Colors.END}").strip().lower()
    if confirm not in ['yes', 'y']:
        print("\nOperation cancelled.")
        sys.exit(0)

    # Step 6: Perform operations
    print_header("\nExecuting operations...")

    # Unbind selected interfaces from current drivers
    for iface in selected_interfaces:
        if not unbind_device(selected_device, iface):
            print_error("Failed to unbind interface. Aborting.")
            sys.exit(1)

    # Unload module if already loaded
    if module_already_loaded:
        if not unload_module(selected_module):
            print_error("Failed to unload existing module.")
            print_warning("You may need to manually unbind all devices using this driver first.")
            sys.exit(1)
        # Give kernel a moment after unloading
        time.sleep(0.3)

    # Load new module
    if not load_module(selected_module):
        print_error("Failed to load module. Attempting to restore...")
        # Try to rebind each selected interface to its original driver
        for iface in selected_interfaces:
            orig = original_interface_drivers.get(iface['name'], 'none')
            if orig != 'none':
                bind_interface_to_driver(iface['name'], orig)
        sys.exit(1)

    # Give kernel time to auto-probe and bind
    print("Waiting for kernel to probe interfaces...")
    time.sleep(0.5)

    # Bind selected interfaces to new driver
    bound_any = False
    for iface in selected_interfaces:
        if bind_device(selected_device, selected_module, iface):
            bound_any = True
        else:
            # Restore interfaces that failed to bind to the new driver
            orig = original_interface_drivers.get(iface['name'], 'none')
            if orig != 'none':
                bind_interface_to_driver(iface['name'], orig)

    if not bound_any:
        print_error("Failed to bind any interface to the new driver.")
        print_warning("All selected interfaces were restored where possible. You might need to reconnect the device.")
        sys.exit(1)

    # Success
    print_header("\nOperation completed successfully!")
    print(f"{Colors.GREEN}Device {selected_device['name']} interface(s) are now using the new driver{Colors.END}")

    # Offer to show new device status
    print("\nVerifying device status...")
    new_devices = get_usb_devices()
    for dev in new_devices:
        if dev['name'] == selected_device['name']:
            for iface in dev.get('interfaces', []):
                driver_color = Colors.GREEN if iface['driver'] != "none" else Colors.YELLOW
                print(f"  Interface {iface['name']}: {driver_color}{iface['driver']}{Colors.END}")
            if not dev.get('interfaces') and dev['driver'] != 'none':
                print(f"  Current driver: {Colors.GREEN}{dev['driver']}{Colors.END}")
            break


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\n\n{Colors.YELLOW}Operation cancelled by user.{Colors.END}")
        sys.exit(0)
    except Exception as e:
        print_error(f"Unexpected error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
