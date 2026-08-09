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
