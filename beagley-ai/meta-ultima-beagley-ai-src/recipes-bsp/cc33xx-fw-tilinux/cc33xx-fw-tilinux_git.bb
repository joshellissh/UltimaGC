SUMMARY = "cc33xx WiFi firmware from ti-linux-firmware (matches the in-kernel driver)"

# Why this exists instead of meta-ti-extras' stock cc33xx-fw:
# The standalone cc33xx-fw recipe (git.ti.com/cc33xx-wlan/cc33xx-fw.git @1.7.0.323)
# ships a cc33xx-conf.bin that this board's 6.12.43-ti kernel cc33xx driver
# rejects -- the driver wants a 1353-byte conf, that package ships 1282 -- so the
# FW download fails at every boot and no wlan0 ever appears (its cc33xx_fw.bin and
# cc33xx_2nd_loader.bin differ too; all three are a matched set). The ti-linux-firmware
# bundle carries a cc33xx set the driver accepts (conf 1353B). Verified on hardware
# 2026-09-14: swapping in these three blobs brings wlan0 up and associates. The
# missing cc33xx-nvs.bin the driver also probes for is a non-fatal warning (-2).
#
# ti-linux-fw.inc is the BSP's shared include (also required by ti-sci-fw/ti-dm-fw/
# etc.), so this tracks the exact same ti-linux-firmware SRCREV as the rest of the
# BSP -- no second fetch, no separate version to drift. It provides SRC_URI, SRCREV,
# S, LICENSE and the git fetch; we only narrow do_install/FILES to the cc33xx files.
require recipes-bsp/ti-linux-fw/ti-linux-fw.inc

PACKAGE_ARCH = "${MACHINE_ARCH}"

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware/ti-connectivity
    install -m 0644 \
        ${S}/ti-connectivity/cc33xx-conf.bin \
        ${S}/ti-connectivity/cc33xx_2nd_loader.bin \
        ${S}/ti-connectivity/cc33xx_fw.bin \
        ${D}${nonarch_base_libdir}/firmware/ti-connectivity/
}

# Narrow the inc's firmware-wide FILES down to just what this recipe installs, so
# it never claims (and conflicts over) the rest of /lib/firmware.
FILES:${PN} = "${nonarch_base_libdir}/firmware/ti-connectivity/cc33xx*"
