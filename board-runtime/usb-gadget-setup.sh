#!/bin/sh
# Configure USB CDC-ACM gadget for /dev/ttyGS0 serial communication.
# Raspberry Pi images may already expose ttyGS0 via the legacy g_serial module;
# keep that path when present, otherwise create a configfs ACM gadget.
set -eu

GADGET_DIR="/sys/kernel/config/usb_gadget/g1"

# Find UDC (USB Device Controller)
UDC_NAME=""
if [ -d /sys/class/udc ]; then
    UDC_NAME=$(ls /sys/class/udc/ 2>/dev/null | awk 'NR==1')
fi

if [ -z "$UDC_NAME" ]; then
    echo "usb-gadget-setup: no UDC found, skipping" >&2
    exit 1
fi

if lsmod 2>/dev/null | awk '{print $1}' | grep -qx g_serial && [ -c /dev/ttyGS0 ]; then
    echo "usb-gadget-setup: using existing g_serial /dev/ttyGS0"
    exit 0
fi

# Already configured?
if [ -f "$GADGET_DIR/UDC" ]; then
    CURRENT_UDC=$(cat "$GADGET_DIR/UDC" 2>/dev/null || echo "")
    if [ -n "$CURRENT_UDC" ]; then
        echo "usb-gadget-setup: already configured on $CURRENT_UDC"
        exit 0
    fi
fi

# Ensure configfs is mounted
if [ ! -d /sys/kernel/config ]; then
    mount -t configfs none /sys/kernel/config 2>/dev/null || true
fi

# Load required kernel modules (may already be built-in)
modprobe libcomposite 2>/dev/null || true
modprobe usb_f_acm 2>/dev/null || true

# Create gadget
mkdir -p "$GADGET_DIR"
echo 0x1d6b > "$GADGET_DIR/idVendor"    # Linux Foundation
echo 0x0104 > "$GADGET_DIR/idProduct"    # Multifunction Composite
echo 0x0100 > "$GADGET_DIR/bcdDevice"
echo 0x0200 > "$GADGET_DIR/bcdUSB"       # USB 2.0

# Device strings
mkdir -p "$GADGET_DIR/strings/0x409"
echo "claw-pet" > "$GADGET_DIR/strings/0x409/manufacturer"
echo "Claw Pet Board" > "$GADGET_DIR/strings/0x409/product"

# Use wlan0 MAC as serial number (fallback to fixed string)
SERIAL=$(cat /sys/class/net/wlan0/address 2>/dev/null | tr -d ':' || echo "000000000000")
echo "$SERIAL" > "$GADGET_DIR/strings/0x409/serialnumber"

# Create ACM function -> /dev/ttyGS0
mkdir -p "$GADGET_DIR/functions/acm.usb0"

# Create configuration
mkdir -p "$GADGET_DIR/configs/c.1/strings/0x409"
echo "CDC ACM" > "$GADGET_DIR/configs/c.1/strings/0x409/configuration"
echo 120 > "$GADGET_DIR/configs/c.1/MaxPower"

# Link function to configuration
ln -sf "$GADGET_DIR/functions/acm.usb0" "$GADGET_DIR/configs/c.1/" 2>/dev/null || true

# Bind to UDC
echo "$UDC_NAME" > "$GADGET_DIR/UDC"

echo "usb-gadget-setup: /dev/ttyGS0 configured on $UDC_NAME"
