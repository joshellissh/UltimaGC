#!/bin/sh
# Format a blank/foreign USB drive as the dashcam DVR drive: whole-disk exFAT
# labeled ULTIMA_DVR (the same by-hand prep documented in DASHCAM.md), then
# re-trigger udev so 99-ultima-dvr.rules fscks + mounts it at /mnt/dvr.
#
# Invoked by ultima-app (DashcamRecorder::formatDrive) when the driver taps
# FORMAT in the on-screen dialog. $1 = a WHOLE-DISK device node (e.g. /dev/sda).
#
# DESTRUCTIVE. The guards below are defense-in-depth: they independently
# re-check, in the thing that actually runs mkfs, every safety property
# ultima-app already checked (detectFormatCandidate) — a wipe never trusts its
# caller. On any failure it prints ONE human-readable line on stderr (which the
# app shows in the dialog) and exits non-zero.
set -u

MKFS=/usr/sbin/mkfs.exfat
BLOCKDEV=/usr/sbin/blockdev
UDEVADM=/usr/sbin/udevadm

fail() { echo "$1" >&2; exit 1; }

DEV="${1:-}"
[ -n "$DEV" ] || fail "No device given."
[ -b "$DEV" ] || fail "Not a block device: $DEV"

name=${DEV#/dev/}
case "$name" in
    */*|"")                        fail "Bad device name: $DEV" ;;
    mmcblk*|nvme*|loop*|ram*|dm-*|sr*) fail "Refusing non-USB device: $DEV" ;;
esac

# Must be a WHOLE disk, not a partition: whole disks have their own /sys/block
# entry, partitions live under it (/sys/block/sda/sda1). Refuse a partition so
# we never mkfs over one partition of a table we were handed as a "disk".
[ -d "/sys/block/$name" ] || fail "Not a whole disk: $DEV"

# The core safety guard: the device's sysfs path must traverse a USB *bus* node
# (usb1, usb2, ... -- e.g. .../xhci-hcd.5.auto/usb1/1-1/.../block/sda, verified
# on this board). This is what excludes the SD card (mmcblk / SDHCI) and
# everything on-SoC. Match "/usb" + a digit, not a bare "/usb/" (there is none:
# the controller dirs are "*.usb").
real=$(readlink -f "/sys/block/$name")
case "$real" in
    */usb[0-9]*) : ;;
    *)           fail "Not a USB device: $DEV" ;;
esac

# Never wipe a drive that already IS the DVR drive (already labeled ULTIMA_DVR)
# ...
if [ -e /dev/disk/by-label/ULTIMA_DVR ]; then
    lbl=$(readlink -f /dev/disk/by-label/ULTIMA_DVR 2>/dev/null)
    case "$lbl" in
        "/dev/$name"|"/dev/$name"[0-9]*|"/dev/$name"p[0-9]*)
            fail "Drive is already set up (ULTIMA_DVR)." ;;
    esac
fi
# ...nor one that is currently mounted (in use).
if grep -qE "^/dev/${name}[0-9p]* " /proc/mounts; then
    fail "Drive is in use (mounted) — unplug and retry."
fi

# --- destructive from here ---
# Whole-disk exFAT, no partition table: systemd-mount mounts the labeled block
# device directly, whole-disk or partition (see 99-ultima-dvr.rules). -n sets
# the volume LABEL the mount rule matches on.
"$MKFS" -n ULTIMA_DVR "$DEV" || fail "mkfs.exfat failed on $DEV."

# Drop any stale partition nodes (sdX1...) left from a previous partition table,
# now that the disk is a single whole-disk filesystem, so udev re-probes clean.
"$BLOCKDEV" --rereadpt "$DEV" 2>/dev/null || true

# Re-run udev's ADD path for the disk: repopulates ID_FS_LABEL and fires
# 99-ultima-dvr.rules (fsck + systemd-mount at /mnt/dvr, and a correct udev db
# so the unplug umount rule matches later). Deliberately NOT done by widening
# the rule to add|change — that would re-fire on every close-after-write,
# including the fsck the rule itself launches.
"$UDEVADM" trigger --action=add --sysname-match="$name" 2>/dev/null || true
"$UDEVADM" settle --timeout=10 2>/dev/null || true

echo "Formatted $DEV as ULTIMA_DVR (exFAT)."
exit 0
