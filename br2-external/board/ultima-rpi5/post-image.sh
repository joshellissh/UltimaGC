#!/bin/bash
set -e

BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"

# Copy our boot config files over the firmware defaults
cp "$BOARD_DIR/config.txt" "$BINARIES_DIR/rpi-firmware/config.txt"
cp "$BOARD_DIR/cmdline.txt" "$BINARIES_DIR/rpi-firmware/cmdline.txt"

# Build the list of boot files dynamically
FILES=()

# Add all DTBs
for i in "${BINARIES_DIR}"/*.dtb; do
    [ -e "$i" ] && FILES+=( "$(basename "$i")" )
done

# Add all rpi-firmware files (config.txt, cmdline.txt, overlays, etc.)
for i in "${BINARIES_DIR}"/rpi-firmware/*; do
    [ -e "$i" ] && FILES+=( "rpi-firmware/$(basename "$i")" )
done

# Add the kernel image
FILES+=( "Image" )

# Generate genimage.cfg from the file list
BOOT_FILES=$(printf '\t\t\t"%s",\n' "${FILES[@]}")
cat > "${GENIMAGE_CFG}" << EOF
image boot.vfat {
	vfat {
		files = {
${BOOT_FILES}
		}
	}
	size = 64M
}

image sdcard.img {
	hdimage {
	}

	partition boot {
		partition-type = 0xC
		bootable = "true"
		image = "boot.vfat"
	}

	partition rootfs {
		partition-type = 0x83
		image = "rootfs.ext4"
	}
}
EOF

# Generate the image
trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"

rm -rf "${GENIMAGE_TMP}"

genimage \
    --rootpath "${ROOTPATH_TMP}" \
    --tmppath "${GENIMAGE_TMP}" \
    --inputpath "${BINARIES_DIR}" \
    --outputpath "${BINARIES_DIR}" \
    --config "${GENIMAGE_CFG}"
