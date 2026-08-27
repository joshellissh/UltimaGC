SUMMARY = "Ultima fullscreen Qt5 QML gauge cluster"
LICENSE = "CLOSED"

DEPENDS = "qtbase qtdeclarative libjpeg-turbo"
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
#
# libjpeg-turbo (2026-08-26, dashcam-recording encode spike): already built
# for qtbase's own jpeg image plugin (libqjpeg.so, confirmed present at
# runtime), but that's qtbase's DEPENDS, not this recipe's — ultima-app needs
# its own explicit DEPENDS to get turbojpeg.h and libturbojpeg.so into ITS
# recipe-sysroot (Yocto sysroots are per-recipe, not transitively shared).
# RDEPENDS pinned too, same can't-silently-drop-out reasoning as the other
# two entries here, even though qtbase-plugins likely already pulls the
# runtime .so in transitively.
#
# openh264 was tried and reverted 2026-08-26 (dashcam H.264 encode spike) —
# real hardware measured software H.264 at ~3.5fps/stream vs a 15fps
# target at 1080p, ~3.5x slower than the turbojpeg path above. Not viable
# on this SoC's Cortex-A53 cores; see git history if revisiting with a
# different library or hardware.
RDEPENDS:${PN} = "qtbase-plugins qtdeclarative-qmlplugins iproute2 ti-img-rogue-driver kernel-module-uvcvideo libjpeg-turbo"

inherit qmake5 systemd

# Same shared, board-agnostic source UltimaGC's Buildroot RPi5/BeaglePlay
# builds used (ultima-app) — bind-mounted read-only
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
    # ULTIMA_APP_EXTERNAL_SRC is a live bind-mount of the source tree on the
    # host's SMB-mounted checkout, which accumulates .smbdeleteAAA* files —
    # stale, sometimes lock-stuck leftovers of macOS smbfs's rename-based
    # delete-on-network-share dance, not real source. copytree's scandir-
    # then-copy is a two-phase process, so one going busy/vanishing between
    # the two phases raises shutil.Error and fails do_unpack outright.
    shutil.copytree(d.getVar('ULTIMA_APP_EXTERNAL_SRC'), s,
                     ignore=shutil.ignore_patterns('.smbdelete*'))
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

# do_configure[nostamp] added 2026-08-20, found the same way as the comment
# above: a build that reported success but silently used a stale Makefile.
# do_unpack's nostamp does force do_compile/do_install/do_package to rerun
# every time, but NOT do_configure — bitbake still considered its own
# cached result valid, so qmake never reran even though do_unpack had just
# copied in newer source. Every previous hot-deploy iteration only ever
# edited the BODY of a file already listed in ultima-app.pro's
# HEADERS/SOURCES, which do_configure genuinely has no reason to care about
# (qmake doesn't need to rerun just because a .cpp's contents changed).
# This one added a NEW file (bluetoothagent.cpp/.h) to HEADERS/SOURCES — the
# first change that actually needed qmake to regenerate the Makefile itself
# — and that's exactly when the gap showed up: do_compile ran against a
# Makefile with no bluetoothagent rule at all, linked a binary that quietly
# lacked the new translation unit, and reported a clean build the whole
# time. Confirmed by comparing timestamps directly in the build volume:
# the freshly re-unpacked bluetoothagent.cpp (do_unpack's copy) postdated
# the generated Makefile (do_configure's output) by over an hour.
do_configure[nostamp] = "1"

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
