# Falcon boot for BeagleY-AI: R5 SPL config fragment (see the fragment file
# for the memory map). Only the k3r5 multiconfig build (MACHINE=beagley-ai-k3r5,
# UBOOT_MACHINE=am67a_beagley_ai_r5_defconfig) is touched; the A53 build is
# unchanged (and unused once falcon is the boot path).
#
# u-boot-ti.inc -> u-boot-mergeconfig.inc merges UBOOT_CONFIG_FRAGMENTS via
# `make <defconfig> <fragment>`, which resolves fragment names against
# ${S}/configs/. A file fetched through SRC_URI lands in ${WORKDIR}, not
# there, so copy it into place before configure runs.
FILESEXTRAPATHS:prepend := "${THISDIR}/u-boot-bb.org:"

SRC_URI:append:beagley-ai-k3r5 = " file://am67a_beagley_ai_r5_falcon.config"
UBOOT_CONFIG_FRAGMENTS:beagley-ai-k3r5 = "am67a_beagley_ai_r5_falcon.config"

do_configure:prepend:beagley-ai-k3r5 () {
    cp ${WORKDIR}/am67a_beagley_ai_r5_falcon.config ${S}/configs/
}
