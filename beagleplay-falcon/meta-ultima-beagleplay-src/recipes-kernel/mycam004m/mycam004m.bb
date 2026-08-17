SUMMARY = "mycam004m quad-camera V4L2 driver — fake/mock backend wired up and enabled by default"
DESCRIPTION = "Builds mycam004m.ko (real MY-CAM004M decoder subdevice driver) and \
mycam004m-fake.ko (static-reference-image test backend) from the same \
out-of-tree Makefile. Only the fake backend is enabled at boot here \
(module autoloaded, select-camera-backend.sh run automatically) — the \
real backend's devicetree overlay is intentionally NOT applied by this \
recipe, since no MY-CAM004M hardware is attached to the target yet; \
mycam004m.ko just sits in the image unused until that changes. See \
~/code/mycam004m/docs/ultima-app-integration.md (source repo, not \
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

SRC_URI = "file://mycam004m-fake.conf file://select-camera-backend.service"
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

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/select-camera-backend.service ${D}${systemd_unitdir}/system/select-camera-backend.service
}

# module.bbclass makes ${PN} an empty meta-package (KERNEL_MODULES_META_PACKAGE)
# that already RDEPENDS on every kernel-module-* package kernel-module-split
# discovers (kernel-module-mycam004m-fake, kernel-module-mycam004m) — adding
# to FILES:${PN} here (not replacing it) keeps that wiring and just attaches
# the non-module bits (firmware, script, boot-time units) to the same package,
# so a single "mycam004m" in IMAGE_INSTALL pulls in everything needed.
FILES:${PN} += "${nonarch_base_libdir}/firmware/mycam004m-fake ${bindir}/select-camera-backend.sh ${sysconfdir}/modules-load.d/mycam004m-fake.conf ${systemd_unitdir}/system/select-camera-backend.service"

SYSTEMD_SERVICE:${PN} = "select-camera-backend.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"
