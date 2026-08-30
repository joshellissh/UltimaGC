SUMMARY = "Ultima boot splash - draws a static image to /dev/fb0 before ultima-app starts"
LICENSE = "CLOSED"

SRC_URI = "file://ultima-splash.c file://ultima-splash.service file://splash.rgbx"
S = "${WORKDIR}"

inherit systemd

# No qtbase/qmake5 needed here (unlike ultima-app.bb) -- this is a plain
# libc-only fbdev client, deliberately kept small enough to fully audit;
# see ultima-splash.c's top comment for why it avoids libdrm too.
do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} ${WORKDIR}/ultima-splash.c -o ultima-splash
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ultima-splash ${D}${bindir}/ultima-splash

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/ultima-splash.service ${D}${systemd_unitdir}/system/ultima-splash.service

    # Headerless raw XRGB8888 blob, converted from repo-root "splash
    # screen.png" -- see ultima-splash.c's top comment for the exact
    # regeneration command and why raw rather than shipping libpng/zlib
    # to decode it on-target.
    install -d ${D}${datadir}/ultima-splash
    install -m 0644 ${WORKDIR}/splash.rgbx ${D}${datadir}/ultima-splash/splash.rgbx
}

SYSTEMD_SERVICE:${PN} = "ultima-splash.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

FILES:${PN} += "${bindir}/ultima-splash ${systemd_unitdir}/system/ultima-splash.service ${datadir}/ultima-splash/splash.rgbx"
