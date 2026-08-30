SUMMARY = "Mount /data from whichever disk (SD or eMMC) root itself booted from"
DESCRIPTION = "wic/ultima-beagley-ai.wks.in adds /data as partition 3. A \
fstab entry keyed on filesystem UUID or LABEL would be byte-identical on \
every card dd'd from the same .wic image, and so ambiguous whenever more \
than one is visible at once. The wks file carries --no-fstab-update on the \
root partition specifically so wic never writes such an entry; this service \
mounts /data itself, by deriving the device from the root filesystem that's \
already resolved by the time it runs, which is unambiguous by construction."
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
