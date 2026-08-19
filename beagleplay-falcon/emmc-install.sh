#!/bin/sh
# Install the falcon image onto the BeaglePlay's eMMC so the board boots the
# gauge cluster with no USR button held.
#
# RUNS ON THE BOARD, booted from the SD card (USR held). Two ways to drive it:
#
#   SSH (preferred — no serial console needed at all). `./emmc-push.sh` on the
#   Mac copies this script plus f.xz/t3e.bin/md5sums.txt into /tmp and runs it.
#   The image has passwordless root dropbear and DHCP ethernet, so the board is
#   reachable at beagleplay-ti.local as soon as it boots.
#
#   Serial console, if the board has no network. `./emmc-serve.sh` on the Mac
#   serves the files over HTTP and prints:
#     wget -qO- http://<mac-ip>:877/emmc-install.sh | sh -s http://<mac-ip>:877
#
# With an argument, it downloads from that base URL. With none, it expects
# f.xz, t3e.bin and md5sums.txt to already be in /tmp.
#
# Staged in /tmp, not /root: root is read-only-rootfs as of the BeaglePlay
# read-only-rootfs work, so /root isn't writable anymore (confirmed the hard
# way — "Read-only file system" on the first line of the SSH path). /tmp is
# tmpfs, plenty of headroom against this payload, and nothing here needs to
# survive a reboot.
#
# Three separate things have to be right for eMMC falcon boot, and each fails
# in its own way (see NOTES.md "eMMC persistence"):
#   1. the eMMC's boot0 *hardware* partition holds the R5 SPL — the TI K3 ROM
#      does not look in the user area for eMMC the way it does for SD;
#   2. that SPL is the eMMC-targeting build (mmcdev=0 / bootpart=0:2), since
#      falcon's boot device is compile-time — see build-emmc-spl.sh;
#   3. the eMMC user area holds a valid filesystem whose ext4 superblock size
#      agrees with the partition table.
#
# Everything it does is verified; it stops at the first failure rather than
# leaving a half-installed eMMC.

set -e
BASE="${1:-}"

LOG=/tmp/emmc-install.log
# fd 3 keeps a handle on the console: everything else (including stderr and the
# set -x trace) goes to the log, so progress has to be echoed to fd 3 explicitly
# or the operator watches a silent console through a multi-minute 950MB write.
exec 3>&2
exec > "$LOG" 2>&1
set -x

say() { set +x; echo "### $*" >&3; echo "### $*"; set -x; }
die() { set +x; echo "### FAILED: $*  (see $LOG)" >&3; echo "### FAILED: $*"; exit 1; }

# --- 0. must be running from the SD, not the eMMC we are about to overwrite ---
ROOT_DEV=$(findmnt -no SOURCE /)
case "$ROOT_DEV" in
    /dev/mmcblk1p*) ;;
    *) die "root is $ROOT_DEV, expected /dev/mmcblk1p2 (SD). Boot the SD card with USR held." ;;
esac

# --- 1. fetch + verify ---
cd /tmp
if [ -n "$BASE" ]; then
    say "downloading from $BASE"
    wget -q "$BASE/f.xz"        -O f.xz        || die "download f.xz"
    wget -q "$BASE/t3e.bin"     -O t3e.bin     || die "download t3e.bin"
    wget -q "$BASE/md5sums.txt" -O md5sums.txt || die "download md5sums.txt"
fi
for f in f.xz t3e.bin md5sums.txt; do
    [ -f "/tmp/$f" ] || die "/tmp/$f missing — pass a base URL, or use emmc-push.sh"
done
md5sum -c md5sums.txt || die "md5 mismatch — transfer was corrupted, re-copy and retry"

# The SPL that goes in boot0 must be the eMMC-targeting build, or the board
# loads it from eMMC and then hunts for the falcon payload on a nonexistent SD
# card ("MMC: no card present").
T3E_MMCDEV=$(strings t3e.bin | grep -E '^mmcdev=' || true)
[ "$T3E_MMCDEV" = "mmcdev=0" ] || die "t3e.bin is not the eMMC SPL variant (got '${T3E_MMCDEV:-none}', need mmcdev=0)"

# --- 1.5. back up /data (mmcblk0p3) before the whole-disk write below wipes it ---
# The image write in step 2 is `dd` of the *entire* disk (f.xz bakes partitions
# 1+2+3 together, not just boot+rootfs), so without this, every reflash silently
# resets /data to whatever blank partition 3 the image ships — discovered the
# hard way (2026-08-19): a reflash wiped a live board's /data/wifi-ap.conf and
# reset /data/odometer.json back to OdoStore's hardcoded default. Best-effort:
# if this is a first-ever flash (no /data filesystem yet) or the old /data won't
# mount for any reason, skip the backup and proceed with a blank /data rather
# than blocking the whole image update on a partition that might already be in
# a bad state.
DATA_BACKUP=/tmp/data-backup.tar
rm -f "$DATA_BACKUP"
mkdir -p /tmp/old-data
if mount -t ext4 -o ro /dev/mmcblk0p3 /tmp/old-data 2>/dev/null; then
    if tar -C /tmp/old-data -cf "$DATA_BACKUP" . 2>/dev/null; then
        say "backed up /data ($(wc -c < "$DATA_BACKUP") bytes) — will restore onto the freshly-flashed partition after the image write"
    else
        rm -f "$DATA_BACKUP"
        say "WARNING: /data mounted but tar failed — proceeding without a backup"
    fi
    umount /tmp/old-data
else
    say "no readable /data on the current eMMC (first flash, or partition not yet formatted) — proceeding without a backup"
fi

# --- 2. eMMC user area ---
# umount by device, not by a guessed /run/media/<label>-<dev> path: systemd's
# auto-mount naming follows the filesystem LABEL (this eMMC's rootfs is
# labeled "root", not "rootfs", so it lands at /run/media/root-mmcblk0p2, not
# rootfs-mmcblk0p2 — a hardcoded path guess silently no-ops here). `umount`
# resolves a device argument via /proc/mounts regardless of where it's
# actually mounted, so this works no matter what label/mountpoint is in use.
umount /dev/mmcblk0p1 2>/dev/null || true
umount /dev/mmcblk0p2 2>/dev/null || true
umount /dev/mmcblk0p3 2>/dev/null || true
if grep -q mmcblk0 /proc/mounts; then die "eMMC still mounted — see /proc/mounts"; fi

say "writing image to /dev/mmcblk0 (~1 min)"
xzcat f.xz | dd of=/dev/mmcblk0 bs=4M conv=fsync || die "dd to /dev/mmcblk0"
sync
# Read verification back off the media, not out of the page cache: conv=fsync
# only flushes at the end and does not invalidate cached pages, so a compare
# without this can report a byte-perfect match that the next boot disagrees with.
echo 3 > /proc/sys/vm/drop_caches

# Verify BEFORE blockdev --rereadpt, not after: this board has its own
# /etc/udev/rules.d/automount.rules that auto-mounts newly-visible partitions
# by label via systemd-mount, and `blockdev --rereadpt` is what makes udev
# notice the freshly-written partitions in the first place. That triggered a
# real fsck + automount of /dev/mmcblk0p2 in testing (confirmed in the
# journal: "Starting File System Check on /dev/mmcblk0p2" then
# "Auto-mount of [/run/media/root-mmcblk0p2] successful"), which updates the
# ext4 superblock's s_wtime/mount-count fields -- and produced a spurious cmp
# mismatch at exactly the s_wtime byte offset (char 135267377 == partition 2
# start + superblock offset 48, to the byte). The image write itself was
# fine; the verification was racing this board's own auto-mounter. Reading
# the raw whole-disk device doesn't need the kernel to have re-scanned the
# partition table, so checking first sidesteps the race rather than needing
# to fight or disable automount.rules, which other workflows rely on.
#
# cmp always exits 1 here with "EOF on -" because the device is larger than the
# image; only a "differ" line means an actual mismatch.
xzcat f.xz | cmp - /dev/mmcblk0 > /tmp/cmp.out 2>&1 || true
if grep -q differ /tmp/cmp.out; then die "eMMC content differs from image: $(cat /tmp/cmp.out)"; fi

blockdev --rereadpt /dev/mmcblk0
sleep 2

# --- 2.5. restore /data onto the freshly-written partition 3 ---
# Partition 3 is guaranteed a valid, mountable, empty ext4 filesystem at this
# point — the .wic image bakes a real (if empty) filesystem for /data at
# build time (see wic/ultima-beagleplay.wks.in), not just reserved space.
if [ -f "$DATA_BACKUP" ]; then
    mkdir -p /tmp/new-data
    mount /dev/mmcblk0p3 /tmp/new-data || die "could not mount freshly-flashed /data to restore the backup"
    tar -C /tmp/new-data -xf "$DATA_BACKUP" || die "restoring /data backup onto the freshly-flashed partition failed"
    umount /tmp/new-data
    say "restored previous /data onto the freshly-flashed partition"
fi

# --- 3. keep the SD's PARTUUID distinct from the eMMC's ---
# An MBR PARTUUID is derived from the 4-byte disk signature at offset 440, and
# the wic builder emits that deterministically — so two devices flashed from the
# same image collide. eMMC (mmc0) finishes probing before SD (mmc1), so on a
# collision the kernel's root=PARTUUID always matches the eMMC and SD-forced
# recovery boots mount the wrong rootfs or panic outright, on every boot,
# regardless of how healthy the card is.
#
# Compared live device against live device, after the eMMC write, rather than
# against the image: either device may have been signature-patched by hand in
# the past, and only what is actually on them now matters.
# `od -An -tx1` is GNU-only: this board's BusyBox od rejects -A/-t outright
# (silently exits nonzero under `set -e`'s pipeline, both sides read as ""
# and compare equal) -- verified by hand: it falsely triggered "SD signature
# collides with the eMMC's" against two genuinely different signatures.
# BusyBox's own `hexdump -e` covers the same "N raw bytes -> hex string" job.
sig() { dd if="$1" bs=512 count=1 2>/dev/null | dd bs=1 skip=440 count=4 2>/dev/null | hexdump -e '4/1 "%02x"'; }
EMMC_SIG=$(sig /dev/mmcblk0)
SD_SIG=$(sig /dev/mmcblk1)
say "disk signature: emmc=$EMMC_SIG sd=$SD_SIG"
if [ "$EMMC_SIG" = "$SD_SIG" ]; then
    say "SD signature collides with the eMMC's — patching SD to deadbee5"
    printf '\xe5\xbe\xad\xde' | dd of=/dev/mmcblk1 bs=1 seek=440 count=4 conv=notrunc
    sync
    echo 3 > /proc/sys/vm/drop_caches
    [ "$(sig /dev/mmcblk1)" = "e5beadde" ] || die "SD signature patch did not stick"
    say "SD PARTUUID is now deadbee5-02 — it changes on reboot, which is expected"
fi

# --- 4. superblock size must agree with the partition table ---
# A stale/larger superblock (e.g. left by a previous first-boot resize) gives
# "EXT4-fs (mmcblk0p2): bad geometry" and an unbootable rootfs.
PART_KB=$(awk '$4=="mmcblk0p2"{print $3}' /proc/partitions)
BC=$(dumpe2fs -h /dev/mmcblk0p2 2>/dev/null | awk -F: '/^Block count:/{print $2+0}')
BS=$(dumpe2fs -h /dev/mmcblk0p2 2>/dev/null | awk -F: '/^Block size:/{print $2+0}')
[ -n "$PART_KB" ] || die "no mmcblk0p2 in /proc/partitions — partition table did not reread"
[ -n "$BC" ] && [ -n "$BS" ] || die "dumpe2fs could not read a superblock from /dev/mmcblk0p2"
FS_KB=$(( BC * BS / 1024 ))
say "mmcblk0p2: partition ${PART_KB}KB vs superblock ${FS_KB}KB"
# A filesystem smaller than its partition is normal and boots fine (trailing
# unused space) -- actual SD/eMMC media of the same nominal size commonly
# differ by a few KB in real sector count, so exact equality is the wrong
# check here. (Root used to be the .wic's last partition, sized "rest of
# disk" at build time, which made this slack the common case; now that /data
# is a real partition 3 after root and resize_rootfs.service is masked --
# see wic/ultima-beagleplay.wks.in -- root stays at its fixed build-time
# size and PART_KB/FS_KB should usually match closely. The slack case can
# still happen from media geometry differences, so the check stays.) Only a
# superblock LARGER than its partition is the real hazard this guards
# against ("bad geometry", unbootable) -- verified against this same eMMC:
# partition 868254KB vs superblock 868252KB, 2KB of harmless slack.
[ "$FS_KB" -le "$PART_KB" ] || die "superblock bigger than partition (${FS_KB} vs ${PART_KB})"

# --- 5. eMMC boot0: the eMMC-targeting R5 SPL ---
# boot0 is read-only by default. PARTITION_CONFIG (EXT_CSD byte 179) ships as
# 0x48 = "boot partition 1 enabled", which is what Linux exposes as boot0, so
# no `mmc bootpart enable` is needed.
T3E_SIZE=$(wc -c < t3e.bin)
[ -w /sys/block/mmcblk0boot0/force_ro ] || die "no /sys/block/mmcblk0boot0 — is this really the eMMC?"
echo 0 > /sys/block/mmcblk0boot0/force_ro
dd if=t3e.bin of=/dev/mmcblk0boot0 bs=1M conv=fsync || die "dd to boot0"
sync
echo 3 > /proc/sys/vm/drop_caches
head -c "$T3E_SIZE" /dev/mmcblk0boot0 | cmp - t3e.bin || die "boot0 content does not match t3e.bin"
echo 1 > /sys/block/mmcblk0boot0/force_ro

# Informational: confirm the eMMC's own boot registers, both expected unchanged.
mmc extcsd read /dev/mmcblk0 2>/dev/null | grep -iE "PARTITION_CONFIG|BOOT_BUS" || true

set +x
say "DONE. Now run:  sync; poweroff"
say "Then power the board back up with USR NOT held — it should boot the"
say "gauge cluster from eMMC. Full log: $LOG"
