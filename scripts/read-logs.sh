#!/bin/bash
#
# Read debug logs from the SD card's FAT32 boot partition (macOS-native)
#

set -e

# Find the FAT32 boot partition on the external SD card
PART=$(diskutil list external | grep "Windows_FAT_32" | awk '{print $NF}')
if [ -z "$PART" ]; then
    echo "No FAT32 partition found on external disk."
    echo "Make sure the SD card is inserted."
    exit 1
fi

# Get mount point (macOS auto-mounts FAT32)
MOUNT=$(diskutil info "$PART" 2>/dev/null | grep "Mount Point" | sed 's/.*: *//')
if [ -z "$MOUNT" ]; then
    echo "Mounting /dev/$PART..."
    diskutil mount "$PART" >/dev/null
    MOUNT=$(diskutil info "$PART" | grep "Mount Point" | sed 's/.*: *//')
fi

echo "Boot partition mounted at: $MOUNT"

echo ""
echo "===== ultima-debug.log ====="
if [ -f "$MOUNT/ultima-debug.log" ]; then
    cat "$MOUNT/ultima-debug.log"
else
    echo "(not found)"
fi

echo ""
echo "===== ultima-app.log ====="
if [ -f "$MOUNT/ultima-app.log" ]; then
    cat "$MOUNT/ultima-app.log"
else
    echo "(not found)"
fi
