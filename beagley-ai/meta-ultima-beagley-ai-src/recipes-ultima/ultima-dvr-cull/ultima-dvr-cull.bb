SUMMARY = "Free-space ring-buffer retention for dashcam recordings"
DESCRIPTION = "A systemd timer (ultima-dvr-cull.timer) fires a oneshot every \
few minutes that deletes the oldest .h264 recordings under /mnt/dvr/ULTIMA \
whenever the drive drops below a free-space threshold — so recording fills the \
stick, then rotates oldest-first, with no per-drive configuration. Deliberately \
separate from ultima-app so neither can take the other down. See DASHCAM.md \
(milestone M2) and the paired ultima-dvr-mount recipe. A no-op when no drive \
is mounted."
LICENSE = "CLOSED"

COMPATIBLE_MACHINE = "beagley-ai"

inherit systemd

SRC_URI = "file://ultima-dvr-cull.sh \
           file://ultima-dvr-cull.service \
           file://ultima-dvr-cull.timer"
S = "${WORKDIR}"

# Enable the timer (which pulls the oneshot service); the service itself is
# installed but not separately enabled.
SYSTEMD_SERVICE:${PN} = "ultima-dvr-cull.timer"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/ultima-dvr-cull.sh ${D}${bindir}/ultima-dvr-cull.sh

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/ultima-dvr-cull.service ${D}${systemd_unitdir}/system/ultima-dvr-cull.service
    install -m 0644 ${WORKDIR}/ultima-dvr-cull.timer   ${D}${systemd_unitdir}/system/ultima-dvr-cull.timer
}

FILES:${PN} += "${bindir}/ultima-dvr-cull.sh \
                ${systemd_unitdir}/system/ultima-dvr-cull.service \
                ${systemd_unitdir}/system/ultima-dvr-cull.timer"
