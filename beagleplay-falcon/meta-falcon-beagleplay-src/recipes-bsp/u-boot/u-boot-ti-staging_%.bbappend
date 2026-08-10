# meta-ti-bsp only wires Falcon Mode support for TI's own EVM machine names
# (am62xx-evm, am62pxx-evm, am62axx-evm, am62xx-lp-evm) in u-boot-ti.inc and
# the *-evm-k3r5.conf machine files. beagleplay-ti/-k3r5 aren't covered by
# that wiring upstream (the falcon boot logic itself is SoC-level, in
# arch/arm/mach-k3, not board-specific) so replicate the same two lines here,
# scoped to this recipe only (a global local.conf PACKAGES:prepend:beagleplay-ti
# was found to trip bitbake's QA pkgvarcheck across unrelated recipes, since
# MACHINE=beagleplay-ti is globally active for the whole default build).
UBOOT_CONFIG_FRAGMENTS:ti-falcon:beagleplay-ti-k3r5 = "k3_r5_falcon.config"
PACKAGES:prepend:beagleplay-ti = "${FALCON_PKG} "

# BeaglePlay's binman devicetree never defines a ti-falcon node (unlike TI's
# own am625-sk board), so tools/binman never assembles tifalcon.bin. Patch it
# in, mirroring k3-am625-sk-binman.dtsi trimmed to the GP-only firmware
# variant BeaglePlay actually builds.
FILESEXTRAPATHS:prepend := "${THISDIR}/u-boot-ti-staging:"
SRC_URI:append = " file://0001-arm-dts-k3-am625-beagleplay-add-falcon-boot-binman-.patch"

# k3_r5_falcon.config disables raw MMC/eMMC boot support
# (CONFIG_SPL_SYS_MMCSD_RAW_MODE, CONFIG_SUPPORT_EMMC_BOOT) "to save space",
# since falcon mode reads tifalcon.bin/fitImage off an ext4 partition and only
# needs filesystem boot. This patch re-enables both, for eMMC boot.
#
# Honest scope note: this is part of the configuration that is hardware-verified
# to boot from eMMC, but it was never independently re-tested with the patch
# removed, and it is NOT what fixed the long ROM-level failure that section
# "eMMC persistence" in NOTES.md describes — that was the falcon *payload*
# (tifalcon.bin, ~995KB) being written into boot0 where the ROM expects the R5
# SPL (tiboot3.bin, ~274KB). Keep it unless you have a reason and a board in
# hand to retest without it.
#
# Patching the fragment file in place is deliberate: a second
# UBOOT_CONFIG_FRAGMENTS entry does not work here. Unlike k3_r5_falcon.config,
# which is a real file already in the u-boot source tree's configs/, a file only
# fetched via SRC_URI never lands in configs/ where the merge-config make rule
# looks for it.
SRC_URI:append = " file://0001-k3_r5_falcon-enable-eMMC-raw-boot-support.patch"

# k3_falcon_fdt_fixup() (arch/arm/mach-k3/common.c) builds the falcon
# kernel cmdline with a fixed snprintf, not from a file — a uEnv.txt/
# bootargs edit can't touch it. Appends "quiet": printk to a 115200-baud
# UART console is synchronous and systemd itself also honors "quiet" to
# suppress its own [ OK ]/Started spam — measured ~3.3s of pure UART wire
# time (38KB of console text) on the critical path before tidss inits on an
# unpatched boot. Tried doing this from the kernel side first
# (CONFIG_CMDLINE_EXTEND) — dead end, that Kconfig choice member doesn't
# exist on this kernel's arm64 Kconfig (only CMDLINE_FROM_BOOTLOADER/
# CMDLINE_FORCE), and CMDLINE_FORCE would replace the whole cmdline with a
# static string, losing the dynamically-derived root=PARTUUID this same
# image needs for both SD and eMMC boot (different PARTUUID each). See
# meta-ultima-beagleplay-src's ultima-display.cfg history for the two
# silent-downgrade rounds that ruled that path out.
SRC_URI:append = " file://0002-k3_r5_falcon-add-quiet-to-falcon-boot-cmdline.patch"
