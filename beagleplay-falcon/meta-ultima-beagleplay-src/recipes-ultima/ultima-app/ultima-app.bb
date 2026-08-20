SUMMARY = "Ultima fullscreen Qt5 QML gauge cluster"
LICENSE = "CLOSED"

DEPENDS = "qtbase qtdeclarative qtconnectivity"
# ti-img-rogue-driver explicit here (not just via tisdk-base-image.bbappend's
# IMAGE_INSTALL) so the GPU module can't silently drop out of the image if
# that list changes later — same reasoning this project already used once
# for kernel-module-tidss before tidss went built-in. See pvrsrvkm.conf below.
#
# kernel-module-uvcvideo (2026-08-14, "Live camera feed"): CONFIG_USB_VIDEO_CLASS=m
# was already =m in arago's own base defconfig — confirmed against the built
# .config, nothing in this layer's own .cfg fragments requested it — and udev
# already autoloads it with no modules-load.d entry needed (unlike pvrsrvkm,
# which is out-of-tree; uvcvideo is in-tree and coldplugs fine, verified on
# hardware with the grabber plugged in). Pinned here for the same
# can't-silently-drop-out-of-the-image reason as ti-img-rogue-driver above,
# not because it's currently missing.
RDEPENDS:${PN} = "qtbase-plugins qtdeclarative-qmlplugins iproute2 ti-img-rogue-driver kernel-module-uvcvideo"

inherit qmake5 systemd

# Same shared, board-agnostic source UltimaGC's Buildroot RPi5/BeaglePlay
# builds used (ultima-app/src) — bind-mounted read-only
# at a fixed container path (see build.sh) and copied into WORKDIR here so
# the build never writes into the shared source tree, mirroring what
# Buildroot's own "local" site method already does for this same package.
ULTIMA_APP_EXTERNAL_SRC = "/home/builder/yocto/ultima-app-src"

SRC_URI = "file://ultima-app.service file://70-can.rules file://pvrsrvkm.conf"
S = "${WORKDIR}/ultima-app-src"

python do_unpack:append() {
    import shutil
    s = d.getVar('S')
    if os.path.exists(s):
        shutil.rmtree(s)
    shutil.copytree(d.getVar('ULTIMA_APP_EXTERNAL_SRC'), s)
}

# do_unpack's task signature is computed from SRC_URI (just the .service and
# .rules files) — nothing hashes the contents of ULTIMA_APP_EXTERNAL_SRC, so
# a source-only edit (change a .qml, change nothing else) leaves every
# task's signature unchanged and bitbake reuses the stale sstate object,
# silently rebuilding an image with the OLD app binary. Confirmed the hard
# way: two full image builds in a row after editing SetTimeScreen.qml/
# main.qml both produced zero "ultima-app" lines in the task log — do_unpack
# never reran, so the workdir's copy of the source (and the compiled binary)
# stayed whatever was last built hours earlier. Same class of trap as the
# KERNEL_CONFIG_FRAGMENTS one this layer already learned the hard way (see
# linux-ti-staging_%.bbappend). nostamp forces do_unpack — and everything
# downstream of it (do_compile, do_install, do_package, ...) — to run every
# single build, which is the only way to make this recipe actually match
# what "always mirror the current host source" already claims to do.
do_unpack[nostamp] = "1"

SYSTEMD_SERVICE:${PN} = "ultima-app.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/ultima-app ${D}${bindir}/ultima-app

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/ultima-app.service ${D}${systemd_unitdir}/system/ultima-app.service

    install -d ${D}${sysconfdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/70-can.rules ${D}${sysconfdir}/udev/rules.d/70-can.rules

    # GPU enablement spike (2026-08-12): pvrsrvkm is out-of-tree, unlike
    # built-in tidss, so nothing coldplugs it automatically. Force it via
    # systemd-modules-load.service instead of relying on udev — see
    # ultima-app.service's After=systemd-modules-load.service for the
    # ordering half of this fix.
    install -d ${D}${sysconfdir}/modules-load.d
    install -m 0644 ${WORKDIR}/pvrsrvkm.conf ${D}${sysconfdir}/modules-load.d/pvrsrvkm.conf

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

    # Persistent odometer state. This is just the empty mountpoint —
    # wic/ultima-beagleplay.wks.in provisions the actual /data partition (p3,
    # mirroring the Buildroot boards' separate /data partition), and
    # ultima-data-mount.bb mounts it here before this service starts.
    install -d ${D}/data
}

FILES:${PN} += "${bindir}/ultima-app ${systemd_unitdir}/system/ultima-app.service ${sysconfdir}/udev/rules.d/70-can.rules ${sysconfdir}/modules-load.d/pvrsrvkm.conf /data"
