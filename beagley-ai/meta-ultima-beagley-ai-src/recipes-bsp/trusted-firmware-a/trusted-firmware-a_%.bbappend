# Falcon boot for BeagleY-AI (R5 SPL -> ATF -> OP-TEE -> kernel; no A53 SPL,
# no U-Boot proper, no GRUB).
#
# TF-A's K3 platform hard-codes where BL33 lives and what it passes in x0
# (plat/ti/k3/platform.mk: PRELOADED_BL33_BASE ?= 0x80080000,
# K3_HW_CONFIG_BASE ?= 0x82000000; plat/ti/common/ti_bl31_setup.c). In the
# stock chain BL33 is the A53 SPL at 0x80080000. In falcon the kernel Image
# is BL33, and this board's uncompressed Image is ~43 MiB including BSS, so
# it cannot sit at 0x80080000 without running over the 0x82000000 DTB slot.
# Move both, using exactly the two overrides meta-ti's own ti-falcon
# DISTROOVERRIDE applies for TI's EVMs (trusted-firmware-a-ti.inc):
#   kernel Image @ 0x82000000 (2 MiB aligned, above ATF @0x80000000 and the
#                              R5 SPL's FIT-header buffer @0x80080000)
#   kernel DTB   @ 0x88000000
# The falcon FIT (tifalcon.bin) loads the kernel/DTB to those addresses; the
# R5 SPL's own relocated stack moves out of the way via
# am67a_beagley_ai_r5_falcon.config (u-boot-bb.org bbappend).
#
# Consequence: bl31 from this build only works with the falcon FIT. A
# tispl.bin built against it (A53 SPL at 0x80080000) will no longer be
# jumped to. The pre-falcon tiboot3.bin/tispl.bin/u-boot.img set is the
# fallback, not a rebuild.
EXTRA_OEMAKE:append:beagley-ai = " PRELOADED_BL33_BASE=0x82000000 K3_HW_CONFIG_BASE=0x88000000"
