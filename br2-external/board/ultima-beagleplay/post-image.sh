#!/bin/bash
set -e

BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"
GENIMAGE_CFG="${BOARD_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# Create empty ext4 data partition image (16MB, journaled) for persistent
# odometer state — same pattern as RPi5's post-image.sh.
dd if=/dev/zero of="${BINARIES_DIR}/data.ext4" bs=1M count=16 2>/dev/null
mkfs.ext4 -F -L data "${BINARIES_DIR}/data.ext4" >/dev/null 2>&1

trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"

rm -rf "${GENIMAGE_TMP}"

genimage \
    --rootpath "${ROOTPATH_TMP}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${GENIMAGE_CFG}"
