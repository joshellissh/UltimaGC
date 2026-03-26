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

# Find external physical disks
DISKS=()
while IFS= read -r line; do
    DISKS+=("$line")
done < <(diskutil list external physical 2>/dev/null | grep '^/dev/disk' | awk '{print $1}')

if [ ${#DISKS[@]} -eq 0 ]; then
    echo "No external disks found. Insert an SD card and try again."
    exit 1
fi

echo "External disks:"
echo ""
for i in "${!DISKS[@]}"; do
    disk="${DISKS[$i]}"
    info=$(diskutil info "$disk" 2>/dev/null)
    name=$(echo "$info" | grep 'Media Name' | sed 's/.*: *//')
    size=$(echo "$info" | grep 'Disk Size' | sed 's/.*: *//' | sed 's/ (.*//')
    printf "  [%d]  %s  —  %s  (%s)\n" "$((i+1))" "$disk" "${name:-Unknown}" "${size:-?}"
done
echo ""

if [ ${#DISKS[@]} -eq 1 ]; then
    read -rp "Press Enter to flash ${DISKS[0]}: "
    DISK="${DISKS[0]}"
else
    read -rp "Select disk [1-${#DISKS[@]}]: " choice
    if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt ${#DISKS[@]} ]; then
        echo "Invalid selection. Aborting."
        exit 1
    fi
    DISK="${DISKS[$((choice-1))]}"
fi

# Safety check
if [[ "$DISK" == "/dev/disk0" || "$DISK" == "/dev/disk1" ]]; then
    echo "ERROR: Refusing to write to $DISK (likely your system disk)."
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
