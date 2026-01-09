#!/bin/bash
# bind_usb_bootmouse.sh
# Usage: sudo ./bind_usb_bootmouse.sh [VID] [PID]
# Example: sudo ./bind_usb_bootmouse.sh 0x2516 0x012f

set -euo pipefail

if [ "$EUID" -ne 0 ]; then
  echo "This script must be run as root (sudo)." >&2
  exit 1
fi

VID_IN=${1:-0x2516}
PID_IN=${2:-0x012f}

# normalize (strip 0x, lowercase, pad to 4 hex digits)
norm_hex() {
  local v=${1,,}
  v=${v#0x}
  printf "%04x" $((16#$v))
}

VID=$(norm_hex "$VID_IN")
PID=$(norm_hex "$PID_IN")

MAPFILE="/var/run/usb_bootmouse_bind_${VID}_${PID}.map"

echo "Looking for USB device VID=$VID PID=$PID..."

found=0
for d in /sys/bus/usb/devices/*; do
  if [ -f "$d/idVendor" ] && [ -f "$d/idProduct" ]; then
    v=$(cat "$d/idVendor")
    p=$(cat "$d/idProduct")
    if [ "$v" = "$VID" ] && [ "$p" = "$PID" ]; then
      found=1
      devdir="$d"
      break
    fi
  fi
done

if [ $found -ne 1 ]; then
  echo "Device not found in sysfs for VID=$VID PID=$PID" >&2
  exit 2
fi

echo "Device found at $devdir"

# Ensure our module is loaded
if ! lsmod | grep -q '^usb_bootmouse'; then
  if ! modprobe usb_bootmouse 2>/dev/null; then
    echo "Attempting to insmod from module file..."
    if [ -f "$(pwd)/usb_bootmouse.ko" ]; then
      insmod "$(pwd)/usb_bootmouse.ko"
    else
      echo "usb_bootmouse module not found. Build the module first." >&2
      exit 3
    fi
  fi
fi

echo "Module usb_bootmouse loaded"

# Prepare map file
rm -f "$MAPFILE"
mkdir -p "$(dirname "$MAPFILE")"

# Iterate over interfaces under the device (like 1-2:1.0)
for ifpath in "$devdir"/*:*; do
  [ -e "$ifpath" ] || continue
  IFNAME=$(basename "$ifpath")

  # find the current driver for this interface
  orig_driver=""
  for drv in /sys/bus/usb/drivers/*; do
    [ -e "$drv/$IFNAME" ] || continue
    orig_driver=$(basename "$drv")
    break
  done

  if [ -z "$orig_driver" ]; then
    echo "Interface $IFNAME has no driver currently; will try to bind usb_bootmouse directly"
  else
    echo "Interface $IFNAME currently bound to driver '$orig_driver'"
    # record original driver
    echo "$IFNAME:$orig_driver" >> "$MAPFILE"

    # unbind original driver
    echo -n "$IFNAME" > "/sys/bus/usb/drivers/$orig_driver/unbind" || {
      echo "Failed to unbind $IFNAME from $orig_driver" >&2
      continue
    }
    echo "Unbound $IFNAME from $orig_driver"
  fi

  # bind our driver
  echo -n "$IFNAME" > /sys/bus/usb/drivers/usb_bootmouse/bind || {
    echo "Failed to bind $IFNAME to usb_bootmouse" >&2
    continue
  }
  echo "Bound $IFNAME to usb_bootmouse"
done

if [ -f "$MAPFILE" ]; then
  echo "Saved mapping to $MAPFILE"
else
  echo "No original driver mappings were saved (device may have had no drivers)."
fi

exit 0
