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

# Disable unused getty on tty1 (we want headless to app)
if [ -f "$TARGET_DIR/etc/inittab" ]; then
    sed -i 's|^tty1|#tty1|' "$TARGET_DIR/etc/inittab"
fi

echo "Post-build script complete."
