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

for log in ultima-debug.log ultima-app.log dmesg.log; do
    echo ""
    echo "===== $log ====="
    if [ -f "$MOUNT/$log" ]; then
        cat "$MOUNT/$log"
    else
        echo "(not found)"
    fi
done
