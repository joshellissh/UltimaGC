#!/bin/bash
set -euo pipefail

echo "=== Ultima BeaglePlay SD Card Flasher ==="

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
IMG_FILE="$PROJECT_DIR/output/sdcard-beagleplay.img"

if [ ! -f "$IMG_FILE" ]; then
    echo "ERROR: $IMG_FILE not found. Build it first (see SETUP-BEAGLEPLAY.md)."
    exit 1
fi

echo "Image: $IMG_FILE ($(ls -lh "$IMG_FILE" | awk '{print $5}'))"
echo ""

# Find physical removable disks — includes built-in SD card readers, which
# diskutil reports as "Internal" even though they're removable media, so
# `external physical` alone misses them.
DISKS=()
while IFS= read -r line; do
    disk="$(echo "$line" | awk '{print $1}')"
    if diskutil info "$disk" 2>/dev/null | grep -q "Removable Media:.*Removable"; then
        DISKS+=("$disk")
    fi
done < <(diskutil list physical 2>/dev/null | grep '^/dev/disk' | awk '{print $1}')

if [ ${#DISKS[@]} -eq 0 ]; then
    echo "No removable disks found. Insert an SD card and try again."
    exit 1
fi

echo "Removable disks:"
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
    read -rp "Press Enter to flash ${DISKS[0]} (ALL DATA ON IT WILL BE ERASED): "
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
echo "Insert the SD card into the BeaglePlay and power on."
echo "See SETUP-BEAGLEPLAY.md's Debugging section for how to check boot progress."
