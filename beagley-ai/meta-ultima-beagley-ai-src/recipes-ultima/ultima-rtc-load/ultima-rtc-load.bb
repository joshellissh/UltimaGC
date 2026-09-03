SUMMARY = "Load the system clock from the onboard DS1340 RTC at boot"
DESCRIPTION = "A boot-time oneshot that sets the system clock from rtc0 (the \
battery-backed DS1340) via hwclock, so dashcam segment filenames and the dash \
clock get a real date. A clean no-op when the RTC holds no valid time — the \
DS1340 needs a coin cell on its backup connector and a one-time set first (see \
DASHCAM.md M3). Replaces the intent of meta-ti's ultima-hwclock-load, which is \
excluded because it hardcodes a different RTC chip (BQ32002) this board does \
not have."
LICENSE = "CLOSED"

COMPATIBLE_MACHINE = "beagley-ai"

inherit systemd

SRC_URI = "file://ultima-rtc-load.sh file://ultima-rtc-load.service"
S = "${WORKDIR}"

SYSTEMD_SERVICE:${PN} = "ultima-rtc-load.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/ultima-rtc-load.sh ${D}${bindir}/ultima-rtc-load.sh

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/ultima-rtc-load.service ${D}${systemd_unitdir}/system/ultima-rtc-load.service
}

FILES:${PN} += "${bindir}/ultima-rtc-load.sh ${systemd_unitdir}/system/ultima-rtc-load.service"
