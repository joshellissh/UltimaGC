SUMMARY = "Ultima fullscreen Qt5 QML gauge cluster"
LICENSE = "CLOSED"

DEPENDS = "qtbase qtdeclarative"
RDEPENDS:${PN} = "qtbase-plugins qtdeclarative-qmlplugins iproute2 kernel-module-tidss"

inherit qmake5 systemd

# Same shared, board-agnostic source UltimaGC's Buildroot RPi5/BeaglePlay
# builds use (br2-external/package/ultima-app/src) — bind-mounted read-only
# at a fixed container path (see build.sh) and copied into WORKDIR here so
# the build never writes into the shared source tree, mirroring what
# Buildroot's own "local" site method already does for this same package.
ULTIMA_APP_EXTERNAL_SRC = "/home/builder/yocto/ultima-app-src"

SRC_URI = "file://ultima-app.service file://70-can.rules file://tidss-modules-load.conf"
S = "${WORKDIR}/ultima-app-src"

python do_unpack:append() {
    import shutil
    s = d.getVar('S')
    if os.path.exists(s):
        shutil.rmtree(s)
    shutil.copytree(d.getVar('ULTIMA_APP_EXTERNAL_SRC'), s)
}

SYSTEMD_SERVICE:${PN} = "ultima-app.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/ultima-app ${D}${bindir}/ultima-app

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/ultima-app.service ${D}${systemd_unitdir}/system/ultima-app.service

    install -d ${D}${sysconfdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/70-can.rules ${D}${sysconfdir}/udev/rules.d/70-can.rules

    # tidss (creates /dev/fb0, which linuxfb needs) ships as a module here
    # and CONFIG_DRM_TIDSS=y gets silently downgraded back to =m by a
    # Kconfig dependency we haven't chased down (see ultima-display.cfg) —
    # force it to load at boot instead of relying on udev coldplug timing,
    # which real hardware showed never actually loading it (ultima-app
    # crash-looped continuously, zero "tidss"/fb0 lines in minutes of boot
    # log — see beagleplay-falcon/NOTES.md).
    install -d ${D}${sysconfdir}/modules-load.d
    install -m 0644 ${WORKDIR}/tidss-modules-load.conf ${D}${sysconfdir}/modules-load.d/tidss.conf

    # Persistent odometer state — a plain rootfs directory for now (the
    # Buildroot boards use a separate /data partition; not replicated here
    # yet, see beagleplay-falcon/NOTES.md follow-ups).
    install -d ${D}/data
}

FILES:${PN} += "${bindir}/ultima-app ${systemd_unitdir}/system/ultima-app.service ${sysconfdir}/udev/rules.d/70-can.rules ${sysconfdir}/modules-load.d/tidss.conf /data"
