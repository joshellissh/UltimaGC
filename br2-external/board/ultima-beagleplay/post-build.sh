#!/bin/bash
set -euo pipefail

BOARD_DIR="$(dirname "$0")"
TARGET_DIR="$1"

# Install extlinux.conf into images/ — genimage.cfg picks it up from there and
# places it at boot.vfat's extlinux/extlinux.conf. Same mechanism upstream
# Buildroot's beagleplay_defconfig uses (board/beagleboard/beagleplay/post-build.sh).
install -m 0644 -D "$BOARD_DIR/extlinux.conf" "$TARGET_DIR/../images/extlinux.conf"

# Ensure wpa_supplicant directory exists
mkdir -p "$TARGET_DIR/etc/wpa_supplicant"

# Ensure app directory exists
mkdir -p "$TARGET_DIR/root/app"

# Disable all gettys (no interactive consoles needed)
if [ -f "$TARGET_DIR/etc/inittab" ]; then
    sed -i 's|^tty1|#tty1|' "$TARGET_DIR/etc/inittab"
    sed -i 's|^ttyS2|#ttyS2|' "$TARGET_DIR/etc/inittab"
fi

# Defer non-essential init scripts to run AFTER S11app
INITD="$TARGET_DIR/etc/init.d"
for script in S01syslogd S01seedrng S02klogd S02sysctl S50crond; do
    if [ -f "$INITD/$script" ]; then
        newname="S20${script#S[0-9][0-9]}"
        mv "$INITD/$script" "$INITD/$newname"
    fi
done

# Remove S41dhcpcd — redundant with S40network's udhcpc
rm -f "$INITD/S41dhcpcd"

echo "Post-build script complete."
