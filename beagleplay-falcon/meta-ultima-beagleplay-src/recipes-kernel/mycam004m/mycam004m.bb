SUMMARY = "mycam004m quad-camera V4L2 driver — real backend wired up and enabled by default"
DESCRIPTION = "Builds mycam004m.ko (real MY-CAM004M decoder subdevice driver) and \
mycam004m-fake.ko (static-reference-image test backend) from the same \
out-of-tree Makefile. MY-CAM004M hardware is attached as of 2026-08-23 \
(see NOTES.md) — the real backend is now enabled at boot (module \
autoloaded alongside the in-tree j721e-csi2rx/cdns-dphy-rx CSI2/D-PHY \
drivers it depends on, select-camera-backend.sh run automatically), with \
the camera devicetree node applied via a compile-time patch to \
k3-am625-beagleplay.dts (Falcon boot never runs fdtoverlays, so the \
driver repo's own runtime .dtso can't be used as-is — see \
0002-arm64-dts-k3-am625-beagleplay-add-mycam004m-camera.patch and \
NOTES.md). mycam004m-fake stays built and force-loaded too, so \
select-camera-backend.sh fake remains a live fallback/dev-testing option. \
Also boots mycam004m-configure-pipeline.service, a one-time media-ctl \
pipeline bring-up step the real backend needs to stream at all (the CSI2 \
bridge/shim entities boot with an unrelated default active format that \
never auto-propagates from mycam004m's own pads) — see that script and \
NOTES.md for the hardware-verified story, including why it's currently \
scoped to stream 0 only. \
See ~/code/mycam004m/docs/ultima-app-integration.md (source repo, not \
copied here) for the full device contract."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=eb723b61539feef013de476e68b5c50a"

inherit module systemd

# Source lives in a separate repo (~/code/mycam004m, sibling of this project's
# own UltimaGC checkout) — bind-mounted read-only by build.sh at a fixed
# container path and copied into WORKDIR here, same external-source pattern
# ultima-app.bb and avm-benchmark.bb already use for their own out-of-layer
# sources (see those recipes' comments for the full reasoning).
MYCAM004M_EXTERNAL_SRC = "/home/builder/yocto/mycam004m-src"

SRC_URI = "file://mycam004m-fake.conf file://mycam004m-real.conf file://select-camera-backend.service file://mycam004m-configure-pipeline.sh file://mycam004m-configure-pipeline.service"
S = "${WORKDIR}/mycam004m-src"

python do_unpack:append() {
    import shutil
    s = d.getVar('S')
    if os.path.exists(s):
        shutil.rmtree(s)
    shutil.copytree(d.getVar('MYCAM004M_EXTERNAL_SRC'), s)
}

# Mirrors ultima-app.bb's do_unpack[nostamp]: nothing else hashes
# MYCAM004M_EXTERNAL_SRC's contents into the task signature, so without this
# a driver-source-only edit would leave do_unpack (and everything
# downstream) unrun, silently reusing a stale .ko.
do_unpack[nostamp] = "1"

# module.bbclass's own EXTRA_OEMAKE sets KERNEL_SRC=${STAGING_KERNEL_DIR},
# but this driver's Makefile reads a variable named KDIR instead (it's a
# plain standalone-Kbuild Makefile, not written against OE's convention —
# see its own top comment). Without this, KDIR falls back to the Makefile's
# own default (/lib/modules/$(uname -r)/build), which doesn't exist in the
# build container and would build against the wrong (host) kernel, if it
# built at all.
EXTRA_OEMAKE += "KDIR=${STAGING_KERNEL_DIR}"

# Firmware install: module.bbclass passes INSTALL_FW_PATH into the kernel's
# own `modules_install` target, but this driver's Makefile has no firmware
# logic of its own (modules_install here is pure stock Kbuild) — so the 4
# static reference frames (already generated and checked into the driver
# repo, see tools/gen_fake_frames.py there) are installed explicitly below.
# select-camera-backend.sh and the two boot-time units are installed the
# same way. Unlike the manual SSH workflow the driver repo's own
# docs/fake-driver-testing.md describes (which has to work around
# /lib/firmware being read-only on a live board), baking the firmware into
# the image here needs no such workaround — it's part of the read-only
# rootfs from boot, never written at runtime.
do_install:append() {
    install -d ${D}${nonarch_base_libdir}/firmware/mycam004m-fake
    install -m 0644 ${S}/firmware/mycam004m-fake/cam1.bin ${D}${nonarch_base_libdir}/firmware/mycam004m-fake/cam1.bin
    install -m 0644 ${S}/firmware/mycam004m-fake/cam2.bin ${D}${nonarch_base_libdir}/firmware/mycam004m-fake/cam2.bin
    install -m 0644 ${S}/firmware/mycam004m-fake/cam3.bin ${D}${nonarch_base_libdir}/firmware/mycam004m-fake/cam3.bin
    install -m 0644 ${S}/firmware/mycam004m-fake/cam4.bin ${D}${nonarch_base_libdir}/firmware/mycam004m-fake/cam4.bin

    install -d ${D}${bindir}
    install -m 0755 ${S}/scripts/select-camera-backend.sh ${D}${bindir}/select-camera-backend.sh

    # mycam004m-fake is out-of-tree, so nothing coldplugs it via udev —
    # same situation, same fix, as pvrsrvkm.conf in ultima-app.bb.
    install -d ${D}${sysconfdir}/modules-load.d
    install -m 0644 ${WORKDIR}/mycam004m-fake.conf ${D}${sysconfdir}/modules-load.d/mycam004m-fake.conf

    # Real backend: mycam004m.ko is out-of-tree (same coldplug gap as
    # -fake above), and j721e-csi2rx/cdns-dphy-rx are in-tree but
    # DT-instantiated platform-device drivers — the same class of udev
    # coldplug gap already hit and fixed for CONFIG_DRM_TIDSS (see
    # NOTES.md), not something to re-risk here. modprobe resolves
    # cdns-csi2rx (j721e-csi2rx's module dependency) and
    # videodev/v4l2-fwnode/mc/v4l2-async (mycam004m's) automatically, so
    # only the three leaf modules need listing.
    install -m 0644 ${WORKDIR}/mycam004m-real.conf ${D}${sysconfdir}/modules-load.d/mycam004m-real.conf

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/select-camera-backend.service ${D}${systemd_unitdir}/system/select-camera-backend.service

    # media-ctl pipeline bring-up (2026-08-23, see NOTES.md): "cdns_csi2rx
    # ...csi-bridge" and "...ticsi2rx" both boot with an uninitialized
    # default active format that doesn't match mycam004m's own (correct)
    # pads, which makes VIDIOC_STREAMON on every /dev/mycam/camN fail with
    # EPIPE until this runs once — see the script's own header comment for
    # the full hardware-verified story and why only stream 0 is configured
    # for now.
    install -m 0755 ${WORKDIR}/mycam004m-configure-pipeline.sh ${D}${bindir}/mycam004m-configure-pipeline.sh
    install -m 0644 ${WORKDIR}/mycam004m-configure-pipeline.service ${D}${systemd_unitdir}/system/mycam004m-configure-pipeline.service
}

# module.bbclass makes ${PN} an empty meta-package (KERNEL_MODULES_META_PACKAGE)
# that already RDEPENDS on every kernel-module-* package kernel-module-split
# discovers (kernel-module-mycam004m-fake, kernel-module-mycam004m) — adding
# to FILES:${PN} here (not replacing it) keeps that wiring and just attaches
# the non-module bits (firmware, script, boot-time units) to the same package,
# so a single "mycam004m" in IMAGE_INSTALL pulls in everything needed.
FILES:${PN} += "${nonarch_base_libdir}/firmware/mycam004m-fake ${bindir}/select-camera-backend.sh ${bindir}/mycam004m-configure-pipeline.sh ${sysconfdir}/modules-load.d/mycam004m-fake.conf ${sysconfdir}/modules-load.d/mycam004m-real.conf ${systemd_unitdir}/system/select-camera-backend.service ${systemd_unitdir}/system/mycam004m-configure-pipeline.service"

SYSTEMD_SERVICE:${PN} = "select-camera-backend.service mycam004m-configure-pipeline.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

# kernel-module-split.bbclass packages every built .ko regardless of
# IMAGE_INSTALL, and this image doesn't pull in a generic "kernel-modules"
# bundle (see tisdk-base-image.bbappend — a curated, trimmed IMAGE_INSTALL
# list) — so j721e-csi2rx/cdns-dphy-rx (the in-tree CSI2 bridge/D-PHY
# drivers the real backend needs, confirmed CONFIG_VIDEO_TI_J721E_CSI2RX=m/
# CONFIG_PHY_CADENCE_DPHY_RX=m in the built .config) would otherwise never
# land on the target rootfs at all. kernel-module-cdns-csi2rx pulls in
# transitively via kernel-module-j721e-csi2rx's own auto-generated RDEPENDS
# (module.bbclass derives that from modinfo's depends= field) — no need to
# list it explicitly.
RDEPENDS:${PN} += "kernel-module-j721e-csi2rx kernel-module-cdns-dphy-rx"

# media-ctl (2026-08-23): the pipeline bring-up step above needs it, and
# this minimal rootfs doesn't otherwise carry v4l-utils. The split
# "media-ctl" package (meta-oe's v4l-utils recipe) rather than the full
# v4l-utils bundle — it doesn't pull in libv4l/qv4l2/Qt, matching this
# image's existing minimal-tooling stance (see e.g. the kmscube/mesa-demos
# removal note above).
RDEPENDS:${PN} += "media-ctl"
