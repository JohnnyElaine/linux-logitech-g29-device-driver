#!/bin/bash
# restore_hid.sh
# Usage: sudo ./restore_hid.sh [VID] [PID]
# Restores original driver bindings saved by bind_usb_bootmouse.sh

set -euo pipefail

if [ "$EUID" -ne 0 ]; then
  echo "This script must be run as root (sudo)." >&2
  exit 1
fi

VID_IN=${1:-0x2516}
PID_IN=${2:-0x012f}

norm_hex() {
  local v=${1,,}
  v=${v#0x}
  printf "%04x" $((16#$v))
}

VID=$(norm_hex "$VID_IN")
PID=$(norm_hex "$PID_IN")

MAPFILE="/var/run/usb_bootmouse_bind_${VID}_${PID}.map"

if [ ! -f "$MAPFILE" ]; then
  echo "Mapping file $MAPFILE not found. Nothing to restore." >&2
  exit 2
fi

echo "Restoring bindings from $MAPFILE"

while IFS=: read -r IFNAME ORIG; do
  [ -n "$IFNAME" ] || continue
  [ -n "$ORIG" ] || continue

  # unbind our driver if it is bound
  if [ -e "/sys/bus/usb/drivers/usb_bootmouse/$IFNAME" ]; then
    echo -n "$IFNAME" > /sys/bus/usb/drivers/usb_bootmouse/unbind || {
      echo "Failed to unbind $IFNAME from usb_bootmouse" >&2
    }
    echo "Unbound $IFNAME from usb_bootmouse"
  else
    echo "usb_bootmouse not bound to $IFNAME"
  fi

  # bind the original driver back
  if [ -d "/sys/bus/usb/drivers/$ORIG" ]; then
    echo -n "$IFNAME" > /sys/bus/usb/drivers/$ORIG/bind || {
      echo "Failed to bind $IFNAME to $ORIG" >&2
      continue
    }
    echo "Bound $IFNAME to $ORIG"
  else
    echo "Original driver $ORIG not present on this system; skipping bind for $IFNAME" >&2
  fi

done < "$MAPFILE"

rm -f "$MAPFILE"

echo "Restoration complete."
exit 0
