#!/bin/bash
set -euo pipefail

echo "=== Ultima RPi5 SD Card Flasher ==="

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
IMG_FILE="$PROJECT_DIR/output/sdcard.img"

if [ ! -f "$IMG_FILE" ]; then
    echo "ERROR: $IMG_FILE not found. Run build.sh first."
    exit 1
fi

echo "Image: $IMG_FILE ($(ls -lh "$IMG_FILE" | awk '{print $5}'))"
echo ""

# List available disks
echo "Available disks:"
diskutil list external physical 2>/dev/null || diskutil list
echo ""

read -rp "Enter SD card device (e.g., /dev/disk4): " DISK

if [ -z "$DISK" ]; then
    echo "No device specified. Aborting."
    exit 1
fi

# Safety check
if [[ "$DISK" == "/dev/disk0" || "$DISK" == "/dev/disk1" ]]; then
    echo "ERROR: Refusing to write to $DISK (likely your system disk)."
    exit 1
fi

echo ""
echo "WARNING: This will ERASE ALL DATA on $DISK"
read -rp "Are you sure? (yes/no): " CONFIRM

if [ "$CONFIRM" != "yes" ]; then
    echo "Aborting."
    exit 1
fi

# Unmount and flash
RDISK="${DISK/disk/rdisk}"
echo "Unmounting $DISK..."
diskutil unmountDisk "$DISK"

echo "Flashing image to $RDISK..."
sudo dd if="$IMG_FILE" of="$RDISK" bs=4m status=progress

echo "Syncing..."
sync

echo "Ejecting..."
diskutil eject "$DISK"

echo ""
echo "=== Flash complete ==="
echo "Insert the SD card into your RPi5 and power on."
echo "The Qt app should appear within ~5-10 seconds."
