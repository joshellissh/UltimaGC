#!/usr/bin/env bash
# Install the falcon image onto the board's eMMC over SSH — no serial console.
#
# The image ships passwordless root dropbear (socket-activated), DHCP ethernet
# via systemd-networkd, and avahi, so a booted board answers on
# beagleplay-ti.local with no setup at all. That makes SSH strictly better than
# the serial path: no ~80-character command-corruption limit, real exit codes,
# and file transfer without an HTTP server.
#
# Usage:
#   ./emmc-push.sh [user@host]        default root@beagleplay-ti.local
#
# The board must be booted FROM THE SD CARD (hold USR through power-on) — this
# overwrites the eMMC, which can't be done while running from it.

set -euo pipefail
cd "$(dirname "$0")"

HOST="${1:-root@beagleplay-ti.local}"
WIC="deploy-falcon/tisdk-base-image-beagleplay-ti.rootfs.wic.xz"
SPL="deploy-falcon/tiboot3-falcon-emmc.bin"

# StrictHostKeyChecking=no + /dev/null known_hosts on purpose: dropbear
# generates a fresh host key on first boot of a freshly-flashed card, so the SD
# and eMMC systems present different keys under the same hostname and mDNS
# name. Pinning them would produce REMOTE HOST IDENTIFICATION HAS CHANGED and a
# hard refusal on every reflash. This is a lab board on a local network with
# passwordless root; there is no secret here for host-key pinning to protect.
SSH_OPTS=(-o ConnectTimeout=10 -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

sh_() { ssh "${SSH_OPTS[@]}" "$HOST" "$@"; }

for f in "$WIC" "$SPL" emmc-install.sh; do
    [ -f "$f" ] || { echo "Missing $f" >&2; exit 1; }
done

# Capture rather than `grep -q`: under `set -o pipefail`, grep -q exits at the
# first match, `strings` takes SIGPIPE (141), and the pipeline reports failure
# even though the match succeeded. grep without -q drains its input.
SPL_MMCDEV=$(strings "$SPL" | grep -E '^mmcdev=' || true)
if [ "$SPL_MMCDEV" != "mmcdev=0" ]; then
    echo "$SPL is not the eMMC variant (got '${SPL_MMCDEV:-none}', expected mmcdev=0)" >&2
    echo "Run ./build-emmc-spl.sh" >&2
    exit 1
fi

echo "=== Connecting to $HOST ==="
ROOT_DEV=$(sh_ 'findmnt -no SOURCE /') || { echo "Can't reach $HOST." >&2; exit 1; }
echo "board root filesystem: $ROOT_DEV"

case "$ROOT_DEV" in
    /dev/mmcblk1p*) ;;
    /dev/mmcblk0p*)
        echo >&2
        echo "REFUSING: the board is booted from eMMC ($ROOT_DEV)." >&2
        echo "This would overwrite the running root filesystem. Boot the SD card" >&2
        echo "instead: hold USR down before applying power and for a couple of" >&2
        echo "seconds after, then re-run." >&2
        exit 1 ;;
    *)  echo "REFUSING: unexpected root device '$ROOT_DEV'." >&2; exit 1 ;;
esac

echo
echo "=== Copying files (~110MB, a minute or so) ==="
# `ssh 'cat > file'` rather than scp: it needs nothing on the far end but a
# shell, so it works regardless of whether scp/sftp-server is installed.
echo "  f.xz"
sh_ 'cat > /root/f.xz' < "$WIC"
echo "  t3e.bin"
sh_ 'cat > /root/t3e.bin' < "$SPL"
echo "  emmc-install.sh"
sh_ 'cat > /root/emmc-install.sh' < emmc-install.sh
{
    printf '%s  f.xz\n'    "$(md5 -q "$WIC")"
    printf '%s  t3e.bin\n' "$(md5 -q "$SPL")"
} | sh_ 'cat > /root/md5sums.txt'

echo
echo "=== Running the install on the board ==="
sh_ 'sh /root/emmc-install.sh'

echo
echo "=== Install finished. Shutting the board down cleanly ==="
# poweroff kills the connection, so a nonzero exit here is expected, not a
# failure. Unclean power-off is what corrupted this board's rootfs three times.
# </dev/null on the backgrounded subshell too: a background job that keeps stdin
# open can hold the SSH channel open and hang the client.
sh_ 'sync; (sleep 1; poweroff) </dev/null >/dev/null 2>&1 &' || true
sleep 3
echo
echo "Board is powering off. Then:"
echo "  1. wait for the LEDs to go dark"
echo "  2. remove the SD card"
echo "  3. power on with NO button held"
echo "  4. the gauge cluster should come up from eMMC"
echo
echo "Verify with:  ssh ${SSH_OPTS[*]} $HOST 'findmnt -no SOURCE /'   -> /dev/mmcblk0p2"
echo "Install log:  /root/emmc-install.log on the board"
