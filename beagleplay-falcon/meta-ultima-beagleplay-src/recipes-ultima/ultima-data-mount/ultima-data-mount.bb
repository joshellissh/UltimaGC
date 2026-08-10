SUMMARY = "Mount /data from whichever disk (SD or eMMC) root itself booted from"
DESCRIPTION = "wic/ultima-beagleplay.wks.in adds /data as partition 3 on both \
the SD and eMMC images, built from the same .wic file — so a fstab entry \
keyed on filesystem UUID or LABEL is byte-identical on both media and \
ambiguous whenever both are visible at once (every emmc-install.sh run, and \
any production eMMC boot with an SD card left in the slot). The wks file \
carries --no-fstab-update on the root partition specifically so wic never \
writes such an entry; this service mounts /data itself, by deriving the \
device from the root filesystem that's already resolved by the time it \
runs, which is unambiguous by construction."
LICENSE = "CLOSED"

inherit systemd

SRC_URI = "file://ultima-data-mount.service file://ultima-data-mount.sh"

SYSTEMD_SERVICE:${PN} = "ultima-data-mount.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/ultima-data-mount.service ${D}${systemd_unitdir}/system/ultima-data-mount.service

    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/ultima-data-mount.sh ${D}${bindir}/ultima-data-mount.sh
}

FILES:${PN} += "${systemd_unitdir}/system/ultima-data-mount.service ${bindir}/ultima-data-mount.sh"
