#!/bin/bash
set -euo pipefail

BOARD_DIR="$(dirname "$0")"
TARGET_DIR="$1"

# Install boot config files
cp "$BOARD_DIR/config.txt" "$TARGET_DIR/../images/" 2>/dev/null || true
cp "$BOARD_DIR/cmdline.txt" "$TARGET_DIR/../images/" 2>/dev/null || true

# Ensure wpa_supplicant directory exists
mkdir -p "$TARGET_DIR/etc/wpa_supplicant"

# Ensure app directory exists
mkdir -p "$TARGET_DIR/root/app"

# Disable all gettys (no interactive consoles needed)
if [ -f "$TARGET_DIR/etc/inittab" ]; then
    sed -i 's|^tty1|#tty1|' "$TARGET_DIR/etc/inittab"
    sed -i 's|^ttyAMA10|#ttyAMA10|' "$TARGET_DIR/etc/inittab"
fi

# Defer non-essential init scripts to run AFTER S11app
# These are Buildroot-provided and can't be removed from overlay,
# so rename them to S20+ so they don't block app startup.
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
