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

# Refuse anything that isn't removable/external whole-disk media.
#
# The obvious test — "is it internal?" — does not work on a Mac with a built-in
# card reader: `diskutil info` reports `Internal: True` for the SDXC reader
# exactly as it does for the system SSD, and there is no `Internal: Yes` line to
# match at all (only `Device Location: Internal`). An earlier version of this
# check scraped that line, extracted the string "Internal", compared it to "Yes",
# and therefore never refused anything — including the system disk. The field
# that actually separates them is RemovableMediaOrExternalDevice.
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

# Give the SD card a disk signature distinct from the eMMC's.
#
# A Linux MBR PARTUUID is derived from the 4-byte disk signature at offset 440,
# and the wic builder emits that deterministically — flash the same image to
# both the SD and the eMMC and they end up with identical PARTUUIDs. The kernel
# takes the first device matching root=PARTUUID=..., and eMMC (mmc0) finishes
# probing before SD (mmc1), so on a collision an SD-forced recovery boot mounts
# the eMMC's rootfs (or panics if it isn't ready) no matter how healthy the card
# is. Falcon's k3_falcon_fdt_fixup() derives root=PARTUUID from the live
# partition table at boot rather than a baked-in string, so patching the flashed
# card is enough — the image itself needs no changes.
#
# The value is deliberately neither the image's stock 076c4a2a nor the deadbeef
# an earlier session patched onto the eMMC by hand — this card has to differ
# from the eMMC under either of those, and there's no way to read the eMMC's
# signature from here.
SD_SIG_LE='\xe5\xbe\xad\xde'   # little-endian on disk => PARTUUID prefix "deadbee5"
echo "Patching MBR disk signature to deadbee5 (keeps SD distinct from eMMC)..."
diskutil unmountDisk "$COOKED" >/dev/null
MBR=$(mktemp -t ultima-mbr)
# Raw disk devices on macOS only accept sector-aligned reads/writes — `dd bs=1`
# straight at /dev/rdiskN silently no-ops. Read a full sector out, patch it in a
# regular file where byte offsets are unrestricted, write the whole sector back.
sudo dd if="$RAW" of="$MBR" bs=512 count=1 2>/dev/null
printf "$SD_SIG_LE" | dd of="$MBR" bs=1 seek=440 count=4 conv=notrunc 2>/dev/null
sudo dd if="$MBR" of="$RAW" bs=512 count=1 2>/dev/null
sync
rm -f "$MBR"

READBACK=$(sudo dd if="$RAW" bs=512 count=1 2>/dev/null | dd bs=1 skip=440 count=4 2>/dev/null | xxd -p)
if [ "$READBACK" != "e5beadde" ]; then
    echo "WARNING: disk signature reads back as '$READBACK', expected 'e5beadde'." >&2
    echo "SD and eMMC may collide on PARTUUID — see NOTES.md before booting both." >&2
fi

echo "Flushing and ejecting..."
sync
diskutil eject "$COOKED"

echo "Done. $COOKED now has: $IMAGE"
