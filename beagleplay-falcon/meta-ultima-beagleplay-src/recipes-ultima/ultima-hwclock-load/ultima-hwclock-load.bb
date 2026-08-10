SUMMARY = "Load system time from the onboard BQ32002 RTC at boot"
DESCRIPTION = "Runs hwclock --hctosys early in boot so the last time \
SystemClock::setTime() wrote through to /dev/rtc0 survives a reboot with \
no network present (the deployed, in-car case) — see systemd_%.bbappend \
in recipes-ultima/boot-trim for the complementary fix that stops \
systemd-timesyncd from overwriting a manual set while network IS present \
(bench/dev only)."
LICENSE = "CLOSED"

inherit systemd

RDEPENDS:${PN} = "util-linux-hwclock"

SRC_URI = "file://ultima-hwclock-load.service"

SYSTEMD_SERVICE:${PN} = "ultima-hwclock-load.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/ultima-hwclock-load.service ${D}${systemd_unitdir}/system/ultima-hwclock-load.service
}

FILES:${PN} += "${systemd_unitdir}/system/ultima-hwclock-load.service"
