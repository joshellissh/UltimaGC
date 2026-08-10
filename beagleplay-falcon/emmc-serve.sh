#!/usr/bin/env bash
# Serve everything emmc-install.sh needs to the board over HTTP, and print the
# one line to type on the serial console.
#
# Files go over the network rather than the serial console on purpose: past
# ~80-90 characters the console reliably drops and duplicates characters, so
# anything longer than a short command gets typed wrong. Short filenames
# (f.xz, t3e.bin) keep the board-side wget line well under that.
#
# Usage: ./emmc-serve.sh   (Ctrl-C to stop)

set -euo pipefail
cd "$(dirname "$0")"

PORT=877
WIC="deploy-falcon/tisdk-base-image-beagleplay-ti.rootfs.wic.xz"
SPL="deploy-falcon/tiboot3-falcon-emmc.bin"

for f in "$WIC" "$SPL" emmc-install.sh; do
    [ -f "$f" ] || { echo "Missing $f" >&2; exit 1; }
done

# The SPL going into the eMMC's boot0 must be the eMMC-targeting build, or the
# board loads the SPL from eMMC and then goes looking for the falcon payload on
# a nonexistent SD card ("MMC: no card present").
# Capture rather than `grep -q`: under `set -o pipefail`, grep -q exits at the
# first match, `strings` takes SIGPIPE (141), and the pipeline reports failure
# even though the match succeeded. grep without -q drains its input.
SPL_MMCDEV=$(strings "$SPL" | grep -E '^mmcdev=' || true)
if [ "$SPL_MMCDEV" != "mmcdev=0" ]; then
    echo "$SPL is not the eMMC variant (got '${SPL_MMCDEV:-none}', expected mmcdev=0)" >&2
    echo "Run ./build-emmc-spl.sh" >&2
    exit 1
fi

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
ln -s "$(pwd)/$WIC" "$STAGE/f.xz"
ln -s "$(pwd)/$SPL" "$STAGE/t3e.bin"
ln -s "$(pwd)/emmc-install.sh" "$STAGE/emmc-install.sh"
{
    printf '%s  f.xz\n'    "$(md5 -q "$WIC")"
    printf '%s  t3e.bin\n' "$(md5 -q "$SPL")"
} > "$STAGE/md5sums.txt"

IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo "<mac-ip>")

cat <<EOF

Serving on http://$IP:$PORT/ — Ctrl-C to stop.

On the board (booted from SD with USR held), run:

    udhcpc -i eth0
    wget -qO- http://$IP:$PORT/emmc-install.sh | sh -s http://$IP:$PORT

Then, once it prints DONE:

    sync; poweroff

and power back up with USR NOT held.

EOF

cd "$STAGE"
exec python3 -m http.server "$PORT"
