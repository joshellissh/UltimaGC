SUMMARY = "Falcon boot FIT (tifalcon.bin) for BeagleY-AI"
DESCRIPTION = "Assembles the FIT the falcon-configured R5 SPL loads in place of \
tispl.bin: ATF (firmware) + OP-TEE + DM firmware + kernel Image + DTB as \
loadables, at the addresses the falcon-address TF-A jumps to. The DTB gets the \
two fixups U-Boot proper would otherwise apply at runtime (kernel cmdline in \
/chosen, and the tfa reserved-memory node moved to where ATF really is). See \
the BeagleY-AI notes ('Falcon on BeagleY-AI') and files/tifalcon.its.in."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

COMPATIBLE_MACHINE = "beagley-ai"
PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit deploy nopackages

DEPENDS = "u-boot-tools-native dtc-native"

SRC_URI = "file://tifalcon.its.in"
S = "${WORKDIR}"

# Kernel cmdline baked into the DTB — nothing downstream of the R5 SPL populates
# /chosen/bootargs anymore. Same line GRUB passed before falcon, minus
# BOOT_IMAGE=, with root= by device instead of PARTUUID: this board has only
# the SD slot (no eMMC), the SD is always mmcblk1, and the MBR disk signature a
# PARTUUID would encode is assigned by wic per build and then re-patched by
# flash.sh — not something a build-time DTB can know.
ULTIMA_FALCON_BOOTARGS ?= "root=/dev/mmcblk1p2 rootwait rootfstype=ext4 console=ttyS2,115200n8 earlycon quiet vt.global_cursor_default=0 ro"
ULTIMA_FALCON_DTB ?= "k3-am67a-beagley-ai.dtb"
ULTIMA_FALCON_DM_FW ?= "ti-dm/j722s/ipc_echo_testb_mcu1_0_release_strip.xer5f"

# NVP6324 / MY-CAM004M CSI0 camera overlay, merged into the DTB below. Falcon
# boots a static DTB with no runtime overlay stage, so the camera DT is baked
# in here. Source of truth is the repo's camdriver/dts, bind-mounted read-only
# into the container by build.sh (same pattern as the ultima-app source). See
# ../../../../camdriver/PLAN.md.
ULTIMA_FALCON_DTBO_SRC ?= "/home/builder/yocto/camdriver-src/dts/k3-am67a-beagley-ai-nvp6324.dtso"

# All inputs come from DEPLOY_DIR_IMAGE (the kernel Image and bl31 are never
# staged into a sysroot), so depend on the producers' deploy tasks...
do_compile[depends] += " \
    virtual/kernel:do_deploy \
    trusted-firmware-a:do_deploy \
    optee-os:do_deploy \
    ti-dm-fw:do_deploy \
"
# ...and fold the inputs' content into this task's hash, so a rebuilt kernel,
# DTB, bl31, bl32 or DM firmware re-assembles the FIT instead of leaving a
# stale one in deploy. ':False' = tolerate absence at parse time (fresh
# build); the depends above guarantee presence at run time.
do_compile[file-checksums] += " \
    ${DEPLOY_DIR_IMAGE}/Image:False \
    ${DEPLOY_DIR_IMAGE}/${ULTIMA_FALCON_DTB}:False \
    ${DEPLOY_DIR_IMAGE}/bl31.bin:False \
    ${DEPLOY_DIR_IMAGE}/optee/bl32.bin:False \
    ${DEPLOY_DIR_IMAGE}/${ULTIMA_FALCON_DM_FW}:False \
    ${ULTIMA_FALCON_DTBO_SRC}:False \
"

do_compile() {
    D="${DEPLOY_DIR_IMAGE}"
    cp "$D/bl31.bin" bl31.bin
    cp "$D/optee/bl32.bin" bl32.bin
    cp "$D/${ULTIMA_FALCON_DM_FW}" dm.xer5f
    cp "$D/Image" Image
    cp "$D/${ULTIMA_FALCON_DTB}" falcon.dtb

    # Bake the NVP6324 / MY-CAM004M CSI0 camera overlay into the DTB (Falcon has
    # no runtime overlay stage). The overlay enables main_i2c2 (+ its pinmux),
    # adds the nvp6324 node, and turns on CSI0's cdns_csi2rx0 / ti_csi2rx0 /
    # dphy0. Raw pinctrl cells let a bare dtc -@ compile it; fdtoverlay merges it
    # against the DTB's __symbols__ (present — the USB1-host overlay relied on the
    # same). A malformed overlay fails the build here rather than at boot.
    dtc -@ -I dts -O dtb -o nvp6324.dtbo "${ULTIMA_FALCON_DTBO_SRC}"
    fdtoverlay -i falcon.dtb -o falcon.dtb.merged nvp6324.dtbo
    mv falcon.dtb.merged falcon.dtb

    # DTB fixups U-Boot proper would otherwise do at runtime:
    # 1. kernel cmdline.
    fdtput -t s falcon.dtb /chosen bootargs "${ULTIMA_FALCON_BOOTARGS}"
    # 2. ATF really lives at 0x80000000 (bb.org CONFIG_K3_ATF_LOAD_ADDR), not
    #    at the 0x9e780000 the upstream DT reserves. U-Boot proper rewrites
    #    this node in ft_system_setup (arch/arm/mach-k3/j722s/j722s_fdt.c);
    #    under EFI/GRUB the kernel used U-Boot's memory map instead, which hid
    #    the mismatch. With the DT authoritative the kernel would hand
    #    0x80000000-0x80080000 out as RAM and corrupt the running ATF.
    fdtput -t x falcon.dtb /reserved-memory/tfa@9e780000 reg 0x0 0x80000000 0x0 0x80000
    # optee@9e800000 already matches CONFIG_K3_OPTEE_LOAD_ADDR / 0x1800000.

    sed -e "s|@BL31@|${S}/bl31.bin|" -e "s|@BL32@|${S}/bl32.bin|" \
        -e "s|@DM@|${S}/dm.xer5f|" -e "s|@IMAGE@|${S}/Image|" \
        -e "s|@DTB@|${S}/falcon.dtb|" \
        "${S}/tifalcon.its.in" > tifalcon.its

    # -E: external data — the R5 SPL reads the FIT metadata to
    # CONFIG_SPL_LOAD_FIT_ADDRESS (0x80080000) and streams each image to its
    # own load address; without -E the whole ~43 MiB would land at 0x80080000,
    # on top of the kernel's 0x82000000 slot. -B 0x1000: page-align each blob.
    mkimage -f tifalcon.its -E -B 0x1000 tifalcon.bin
    mkimage -l tifalcon.bin
}

do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0644 tifalcon.bin ${DEPLOYDIR}/tifalcon.bin
    # For inspection only (fdtget / mkimage -l); not on the boot partition.
    install -m 0644 falcon.dtb ${DEPLOYDIR}/k3-am67a-beagley-ai-falcon.dtb
    install -m 0644 tifalcon.its ${DEPLOYDIR}/tifalcon.its
}
addtask deploy after do_compile before do_build
