#!/usr/bin/env bash
# Flash a .wic.xz image to an SD card on macOS.
#
# Usage: ./flash.sh /dev/diskN [image.wic.xz]
#   image.wic.xz defaults to deploy-falcon/tisdk-base-image-beagleplay-ti.rootfs.wic.xz
#
# Run with no arguments to just list disks (nothing is written).

set -euo pipefail
cd "$(dirname "$0")"

DEFAULT_IMAGE="deploy-falcon/tisdk-base-image-beagleplay-ti.rootfs.wic.xz"
DISK="${1:-}"
IMAGE="${2:-$DEFAULT_IMAGE}"

if [ -z "$DISK" ]; then
    echo "Usage: $0 /dev/diskN [image.wic.xz]"
    echo
    echo "Available disks:"
    diskutil list
    exit 1
fi

# Normalize to the raw (rdisk) device for faster writes, but keep the
# cooked name around for diskutil info/unmount calls.
DISK="${DISK%/}"
DISK="${DISK#/dev/}"
case "$DISK" in
    rdisk*) COOKED="disk${DISK#rdisk}" ;;
    disk*)  COOKED="$DISK" ;;
    [0-9]*) COOKED="disk${DISK}" ;;
    *) echo "Expected something like disk4, /dev/disk4, or just 4 — got: $DISK" >&2; exit 1 ;;
esac
RAW="/dev/r${COOKED}"
COOKED="/dev/${COOKED}"

if [ ! -f "$IMAGE" ]; then
    echo "Image not found: $IMAGE" >&2
    exit 1
fi

echo "=== Target disk info ==="
diskutil info "$COOKED"
echo
echo "=== Image ==="
ls -la "$IMAGE"
echo

IS_INTERNAL=$(diskutil info "$COOKED" | awk -F': *' '/Internal/{print $2}')
if [ "$IS_INTERNAL" = "Yes" ]; then
    echo "REFUSING: $COOKED is reported as an INTERNAL disk. This script only" >&2
    echo "flashes external/removable media. Aborting." >&2
    exit 1
fi

echo "This will ERASE ALL DATA on $COOKED and write $IMAGE to it."

echo "Unmounting $COOKED..."
diskutil unmountDisk "$COOKED"

echo "Writing image (this can take a few minutes)..."
xzcat "$IMAGE" | sudo dd of="$RAW" bs=4m status=progress

echo "Flushing and ejecting..."
sync
diskutil eject "$COOKED"

echo "Done. $COOKED now has: $IMAGE"
