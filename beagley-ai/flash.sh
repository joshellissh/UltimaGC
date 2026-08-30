#!/usr/bin/env bash
# Flash a .wic.xz image to an SD card on macOS (BeagleY-AI boots from SD; it
# has no onboard eMMC).
#
# Usage: ./flash.sh /dev/diskN [image.wic.xz]
#   image.wic.xz defaults to
#   deploy-beagley-ai/tisdk-base-image-beagley-ai.rootfs.wic.xz
#
# Run with no arguments to just list disks (nothing is written).
#
# This flasher does NOT patch the MBR disk signature. Such a patch would only
# be needed to keep an SD card's PARTUUID distinct from an eMMC's on a board
# that has both; BeagleY-AI has no eMMC, and its image
# bakes a static root=PARTUUID at build time (confirmed by extracting p1 from
# the built .wic) with no Falcon-style live fixup to re-derive it — so
# patching the signature here would desync it from that baked-in value and the
# kernel would fail to find root.

set -euo pipefail
cd "$(dirname "$0")"

DEFAULT_IMAGE="deploy-beagley-ai/tisdk-base-image-beagley-ai.rootfs.wic.xz"
DISK="${1:-}"
IMAGE="${2:-$DEFAULT_IMAGE}"

if [ -z "$DISK" ]; then
    echo "Usage: $0 /dev/diskN [image.wic.xz]"
    echo
    echo "Available disks:"
    diskutil list
    exit 1
fi

# Normalize to the raw (rdisk) device for faster writes, but keep the cooked
# name around for diskutil info/unmount calls.
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

# Refuse anything that isn't removable/external whole-disk media.
#
# The obvious test — "is it internal?" — does not work on a Mac with a
# built-in card reader: `diskutil info` reports `Internal: True` for the SDXC
# reader exactly as it does for the system SSD, and there is no `Internal: Yes`
# line to match at all (only `Device Location: Internal`). An earlier version
# of this check scraped that line, extracted the string "Internal", compared
# it to "Yes", and therefore never refused anything — including the system
# disk. The field that actually separates them is
# RemovableMediaOrExternalDevice.
PLIST=$(mktemp -t ultima-diskinfo)
diskutil info -plist "$COOKED" > "$PLIST" 2>/dev/null
SAFE=$(python3 -c "
import plistlib
p = plistlib.load(open('$PLIST','rb'))
print('yes' if p.get('RemovableMediaOrExternalDevice') and p.get('WholeDisk') else 'no')
" 2>/dev/null || echo no)
rm -f "$PLIST"
if [ "$SAFE" != "yes" ]; then
    echo "REFUSING: $COOKED is not removable/external whole-disk media." >&2
    echo "This script only flashes SD cards and similar. Aborting." >&2
    exit 1
fi

echo "This will ERASE ALL DATA on $COOKED and write $IMAGE to it."

echo "Unmounting $COOKED..."
diskutil unmountDisk "$COOKED"

echo "Writing image (this can take a few minutes)..."
xzcat "$IMAGE" | sudo dd of="$RAW" bs=4m status=progress
sync

echo "Flushing and ejecting..."
sync
diskutil eject "$COOKED"

echo "Done. $COOKED now has: $IMAGE"
