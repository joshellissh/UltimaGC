#!/usr/bin/env bash
# Assemble tifalcon.bin (falcon boot FIT) for BeagleY-AI from the Yocto deploy
# dir inside the falcon-yocto-build Docker volume, using the U-Boot build's own
# mkimage/fdtput. Bring-up/bench tool: the Yocto-integrated equivalent is the
# ultima-falcon-fit recipe (see NOTES.md "Falcon on BeagleY-AI").
#
# Usage:  beagley-ai/falcon/build-tifalcon.sh [outdir]
#   BOOTARGS="..."  override the kernel cmdline baked into the DTB
#
# Outputs (outdir, default beagley-ai/deploy-beagley-ai/falcon/):
#   tifalcon.bin               the FIT the falcon R5 SPL loads instead of tispl.bin
#   tiboot3-falcon.bin         R5 SPL built with am67a_beagley_ai_r5_falcon.config
#   k3-am67a-beagley-ai-falcon.dtb  the patched DTB embedded in the FIT (for inspection)
#   tifalcon.its               the resolved .its
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../deploy-beagley-ai/falcon}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

# Same cmdline GRUB passes today minus BOOT_IMAGE=. root=PARTUUID is the MBR
# disk signature + partition index; the kernel resolves it without any
# bootloader help (block/partitions/msdos.c), same as it does via GRUB now.
BOOTARGS="${BOOTARGS:-root=PARTUUID=076c4a2a-02 rootwait rootfstype=ext4 console=ttyS2,115200n8 earlycon quiet vt.global_cursor_default=0 ro}"

docker run --rm \
  -v falcon-yocto-build:/src:ro \
  -v "$HERE/../meta-ultima-beagley-ai-src/recipes-bsp/ultima-falcon-fit/files:/falcon:ro" \
  -v "$OUT:/out" \
  -e "BOOTARGS=$BOOTARGS" \
  falcon-yocto:latest bash -c '
set -euo pipefail
Y=/src/tisdk/build-beagley-ai
D=$Y/deploy-ti/images/beagley-ai
A53=$Y/arago-tmp-default-glibc/work/beagley_ai-oe-linux/u-boot-bb.org/2025.10+git
MKIMAGE=$A53/build/tools/mkimage
FDTPUT=$A53/recipe-sysroot-native/usr/bin/fdtput
FDTGET=$A53/recipe-sysroot-native/usr/bin/fdtget
export PATH="$A53/recipe-sysroot-native/usr/bin:$PATH"   # mkimage -f runs dtc
W=$(mktemp -d)
cd "$W"

cp "$D/bl31.bin" bl31.bin
cp "$D/optee/bl32.bin" bl32.bin
cp "$D/ti-dm/j722s/ipc_echo_testb_mcu1_0_release_strip.xer5f" dm.xer5f
cp "$D/Image" Image
cp "$D/k3-am67a-beagley-ai.dtb" falcon.dtb

# Sanity: the bl31 in deploy must be the falcon-address build (see the
# trusted-firmware-a bbappend). Its do_compile log records the make vars.
TFA_LOG=$(ls $Y/arago-tmp-default-glibc/work/beagley_ai-oe-linux/trusted-firmware-a/*/temp/log.do_compile | head -1)
grep -q "PRELOADED_BL33_BASE=0x82000000" "$TFA_LOG" || { echo "bl31 was not built with PRELOADED_BL33_BASE=0x82000000 — rebuild trusted-firmware-a first" >&2; exit 1; }
grep -q "K3_HW_CONFIG_BASE=0x88000000" "$TFA_LOG" || { echo "bl31 was not built with K3_HW_CONFIG_BASE=0x88000000" >&2; exit 1; }

# --- DTB fixups U-Boot proper would otherwise do at runtime ---
# 1. Kernel cmdline: nothing downstream populates /chosen/bootargs anymore.
"$FDTPUT" -t s falcon.dtb /chosen bootargs "$BOOTARGS"
# 2. ATF really lives at 0x80000000 (CONFIG_K3_ATF_LOAD_ADDR), not at the
#    0x9e780000 the upstream DT reserves. U-Boot proper rewrites this node in
#    ft_system_setup (arch/arm/mach-k3/j722s/j722s_fdt.c); under EFI/GRUB the
#    kernel took U-Boot'"'"'s memory map instead, which hid the mismatch. With
#    the DT authoritative, the kernel would hand 0x80000000-0x80080000 out as
#    RAM and corrupt the running ATF. Size 0x80000 matches that fixup.
"$FDTPUT" -t x falcon.dtb /reserved-memory/tfa@9e780000 reg 0x0 0x80000000 0x0 0x80000
# optee@9e800000 already matches CONFIG_K3_OPTEE_LOAD_ADDR / 0x1800000.

echo "--- patched DTB:"
echo -n "  bootargs: "; "$FDTGET" falcon.dtb /chosen bootargs
echo -n "  tfa reg:  "; "$FDTGET" -t x falcon.dtb /reserved-memory/tfa@9e780000 reg
echo -n "  optee reg:"; "$FDTGET" -t x falcon.dtb /reserved-memory/optee@9e800000 reg

sed -e "s|@BL31@|$W/bl31.bin|" -e "s|@BL32@|$W/bl32.bin|" -e "s|@DM@|$W/dm.xer5f|" \
    -e "s|@IMAGE@|$W/Image|" -e "s|@DTB@|$W/falcon.dtb|" /falcon/tifalcon.its.in > tifalcon.its

# -E: external data (see the .its header). -B 0x1000: page-align each blob in
# the file so the SPL'"'"'s DMA-backed FAT reads stay aligned.
"$MKIMAGE" -f tifalcon.its -E -B 0x1000 tifalcon.bin >/dev/null
echo "--- tifalcon.bin:"; "$MKIMAGE" -l tifalcon.bin

cp tifalcon.bin /out/tifalcon.bin
cp falcon.dtb /out/k3-am67a-beagley-ai-falcon.dtb
cp tifalcon.its /out/tifalcon.its
cp "$D/tiboot3.bin" /out/tiboot3-falcon.bin
ls -la /out
'
echo "Done: $OUT"
