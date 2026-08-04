#!/bin/bash
set -euo pipefail

echo "=== Ultima RPi5 Buildroot Build ==="

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
VM_SSH="ubuntu@orb"
VM_HOME="$(ssh "$VM_SSH" 'echo $HOME')"
VM_PROJECT_DIR="${VM_HOME}/ultima"

# Sync project files to VM
echo "Syncing project files to OrbStack VM..."
ssh "$VM_SSH" "mkdir -p $VM_PROJECT_DIR"
rsync -av --exclude='buildroot/' --exclude='.git/' --exclude='output/' \
    "$PROJECT_DIR/" "${VM_SSH}:${VM_PROJECT_DIR}/"

# Run build inside VM
echo "Starting Buildroot build inside VM..."
ssh "$VM_SSH" bash -s "$VM_PROJECT_DIR" <<'REMOTE_SCRIPT'
set -euo pipefail
VM_PROJECT_DIR="$1"
BUILDROOT_DIR="$VM_PROJECT_DIR/buildroot"
BR2_EXTERNAL="$VM_PROJECT_DIR/br2-external"
OUTPUT_DIR="$BUILDROOT_DIR/output"

if [ ! -d "$BUILDROOT_DIR" ]; then
    echo "ERROR: Buildroot not found. Run setup-vm.sh first."
    exit 1
fi

cd "$BUILDROOT_DIR"

# Load defconfig with external tree
echo "Loading ultima_rpi5_defconfig..."
make BR2_EXTERNAL="$BR2_EXTERNAL" ultima_rpi5_defconfig

# Build
echo "Building (this will take a while on first run)..."
make -j$(nproc)

echo "=== Build complete ==="
echo "Output image: $OUTPUT_DIR/images/sdcard.img"
ls -lh "$OUTPUT_DIR/images/sdcard.img" 2>/dev/null || echo "Warning: sdcard.img not found"
REMOTE_SCRIPT

# Copy image back to host
echo "Copying sdcard.img back to host..."
OUTPUT_IMG="$PROJECT_DIR/output/sdcard.img"
mkdir -p "$PROJECT_DIR/output"
scp "${VM_SSH}:${VM_PROJECT_DIR}/buildroot/output/images/sdcard.img" "$OUTPUT_IMG" 2>/dev/null && \
    echo "Image saved to: $OUTPUT_IMG" && \
    ls -lh "$OUTPUT_IMG" || \
    echo "Warning: Could not copy image. Check build output above."
