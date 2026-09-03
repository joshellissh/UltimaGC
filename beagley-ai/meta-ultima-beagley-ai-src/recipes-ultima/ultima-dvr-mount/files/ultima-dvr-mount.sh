#!/bin/sh
# Preen-fsck the dashcam USB drive, then mount it. Invoked non-blocking (via
# systemd-run, so a slow fsck never stalls the udev worker) from the add rule
# in 99-ultima-dvr.rules. $1 = the block device (e.g. /dev/sda1).
#
# WHY fsck EVERY mount: exFAT has no journal, and this drive is power-cut
# constantly in a car (the whole recorder is built around losing power). A
# preen repair before each mount fixes the lost clusters / dirty state that
# would otherwise accumulate into an unmountable filesystem. See DASHCAM.md M3.
set -u

DEV="$1"
[ -b "$DEV" ] || exit 0

# -p: repair automatically whatever is safe, no prompt. A drive too corrupt for
# that still gets mounted below (errors=remount-ro keeps it safe) so recording
# can at least attempt to continue, rather than silently not mounting at all.
fsck.exfat -p "$DEV" || logger -t ultima-dvr "fsck.exfat -p $DEV exited $?"

exec systemd-mount --collect \
    -o noatime,nosuid,nodev,noexec "$DEV" /mnt/dvr
