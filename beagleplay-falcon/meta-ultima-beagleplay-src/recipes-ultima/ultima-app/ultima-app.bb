SUMMARY = "Ultima fullscreen Qt5 QML gauge cluster"
LICENSE = "CLOSED"

DEPENDS = "qtbase qtdeclarative"
RDEPENDS:${PN} = "qtbase-plugins qtdeclarative-qmlplugins iproute2"

inherit qmake5 systemd

# Same shared, board-agnostic source UltimaGC's Buildroot RPi5/BeaglePlay
# builds use (br2-external/package/ultima-app/src) — bind-mounted read-only
# at a fixed container path (see build.sh) and copied into WORKDIR here so
# the build never writes into the shared source tree, mirroring what
# Buildroot's own "local" site method already does for this same package.
ULTIMA_APP_EXTERNAL_SRC = "/home/builder/yocto/ultima-app-src"

SRC_URI = "file://ultima-app.service file://70-can.rules"
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

    # tidss used to ship as a module here (CONFIG_DRM_TIDSS=m), and
    # /etc/modules-load.d/tidss.conf + a kernel-module-tidss RDEPENDS forced
    # it to load at boot rather than relying on udev coldplug, which real
    # hardware showed never actually loading it (ultima-app crash-looped,
    # zero "tidss"/fb0 lines in minutes of boot log — see
    # beagleplay-falcon/NOTES.md). Root-caused and fixed properly 2026-08-09:
    # CONFIG_DRM=m was capping it — see ultima-display.cfg. tidss is now
    # genuinely built into the kernel (verified against the built .config),
    # so there's no module to force-load and no kernel-module-tidss package
    # to RDEPEND on anymore — removing both was required, not just cleanup:
    # do_rootfs failed outright ("nothing provides kernel-module-tidss")
    # once that package stopped being produced.

    # Persistent odometer state — a plain rootfs directory for now (the
    # Buildroot boards use a separate /data partition; not replicated here
    # yet, see beagleplay-falcon/NOTES.md follow-ups).
    install -d ${D}/data
}

FILES:${PN} += "${bindir}/ultima-app ${systemd_unitdir}/system/ultima-app.service ${sysconfdir}/udev/rules.d/70-can.rules /data"
