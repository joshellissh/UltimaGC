SUMMARY = "MYIR MY-CAM004M V4L2 decoder driver, plus its fake test backend"
DESCRIPTION = "Out-of-tree V4L2 subdevice driver for the MYIR MY-CAM004M \
quad-AHD-camera decoder (real hardware, CSI0 — see \
recipes-kernel/linux/linux-bb.org/0002-arm64-dts-k3-am67a-beagley-ai-add-mycam004m-csi0.patch \
for the devicetree side), plus mycam004m-fake, a self-contained module that \
serves 4 static reference images as real /dev/video* nodes so ultima-app's \
camera code can be exercised without MY-CAM004M hardware. The real backend \
is the boot default since 2026-08-31 (bench-verified streaming on this \
board): modules force-loaded incl. the in-tree CSI stack, fps=25 modprobe \
options for the attached 25fps AHD camera, and \
mycam004m-configure-pipeline.service to wait out the async media graph, \
resolve /dev/mycam/cam1..4, and set the CSI subdev formats STREAMON needs. \
Source of truth for the driver is the separate mycam004m repo \
(~/code/mycam004m) — see its README's \"BeagleY-AI port\" section for the \
hardware-verification status."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=eb723b61539feef013de476e68b5c50a"

inherit module systemd

# Same external-bind-mount pattern as ultima-app.bb, for the same reason:
# mycam004m is a separate repo (~/code/mycam004m on the host), not something
# this layer vendors or fetches via SRC_URI. See build.sh for the bind mount
# that makes /home/builder/yocto/mycam004m-src exist inside the container.
MYCAM004M_EXTERNAL_SRC = "/home/builder/yocto/mycam004m-src"

SRC_URI = "file://mycam004m-select-backend.sh file://mycam004m-select-backend.service file://mycam004m.conf \
           file://mycam004m-options.conf file://mycam004m-configure-pipeline.sh file://mycam004m-configure-pipeline.service"
S = "${WORKDIR}/mycam004m-src"

python do_unpack:append() {
    import shutil
    s = d.getVar('S')
    if os.path.exists(s):
        shutil.rmtree(s)
    # Same .smbdelete* guard as ultima-app.bb's do_unpack:append — cheap
    # insurance even though ~/code/mycam004m isn't on the SMB-mounted
    # checkout this project's own source tree lives on.
    shutil.copytree(d.getVar('MYCAM004M_EXTERNAL_SRC'), s,
                     ignore=shutil.ignore_patterns('.smbdelete*'))
}

# Same reasoning as ultima-app.bb's do_unpack[nostamp]: nothing hashes
# MYCAM004M_EXTERNAL_SRC's contents, so a source-only edit in ~/code/mycam004m
# would otherwise leave every downstream task's signature unchanged and
# bitbake would silently reuse a stale sstate build.
do_unpack[nostamp] = "1"

# The driver's own Makefile (see its own comment) only recognises KDIR, not
# module.bbclass's KERNEL_SRC — force it via EXTRA_OEMAKE rather than
# patching the Makefile, which stays untouched and identical to what the
# mycam004m repo's own README build instructions produce.
EXTRA_OEMAKE += "KDIR=${STAGING_KERNEL_DIR}"

# module.bbclass's kernel-module-split leaves this (PN's own, umbrella)
# package empty and RDEPENDS it on whichever kernel-module-* subpackages
# actually got built (both mycam004m.ko and mycam004m-fake.ko here) — see
# KERNEL_MODULES_META_PACKAGE in module.bbclass. IMAGE_INSTALL only needs
# to name "mycam004m" for both to land.
do_install:append() {
    install -d ${D}${bindir}
    install -m 0755 ${S}/scripts/select-camera-backend.sh ${D}${bindir}/select-camera-backend.sh
    install -m 0755 ${WORKDIR}/mycam004m-select-backend.sh ${D}${bindir}/mycam004m-select-backend.sh

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/mycam004m-select-backend.service ${D}${systemd_unitdir}/system/mycam004m-select-backend.service

    install -d ${D}${sysconfdir}/modules-load.d
    install -m 0644 ${WORKDIR}/mycam004m.conf ${D}${sysconfdir}/modules-load.d/mycam004m.conf

    # modprobe options for the real backend — fps=25 for the attached 25fps
    # AHD camera (see the file's own comment). Read by modprobe when
    # systemd-modules-load autoloads mycam004m per the conf above.
    install -d ${D}${sysconfdir}/modprobe.d
    install -m 0644 ${WORKDIR}/mycam004m-options.conf ${D}${sysconfdir}/modprobe.d/mycam004m-options.conf

    # Real-backend boot bring-up: bounded wait for the async CSI media graph,
    # symlink re-resolve, and the two media-ctl subdev-format writes without
    # which every VIDIOC_STREAMON fails with EPIPE (bench-verified 2026-08-31
    # — see the script's own header and ~/code/mycam004m docs/testing.md).
    install -m 0755 ${WORKDIR}/mycam004m-configure-pipeline.sh ${D}${bindir}/mycam004m-configure-pipeline.sh
    install -m 0644 ${WORKDIR}/mycam004m-configure-pipeline.service ${D}${systemd_unitdir}/system/mycam004m-configure-pipeline.service

    # mycam004m-fake's static reference frames — see mycam004m-fake.c's
    # request_firmware() call and tools/gen_fake_frames.py in that repo.
    # INSTALL_FW_PATH (module.bbclass's own module_do_install) already
    # points firmware at this same ${nonarch_base_libdir}/firmware tree.
    install -d ${D}${nonarch_base_libdir}/firmware/mycam004m-fake
    install -m 0644 ${S}/firmware/mycam004m-fake/cam1.bin ${D}${nonarch_base_libdir}/firmware/mycam004m-fake/cam1.bin
    install -m 0644 ${S}/firmware/mycam004m-fake/cam2.bin ${D}${nonarch_base_libdir}/firmware/mycam004m-fake/cam2.bin
    install -m 0644 ${S}/firmware/mycam004m-fake/cam3.bin ${D}${nonarch_base_libdir}/firmware/mycam004m-fake/cam3.bin
    install -m 0644 ${S}/firmware/mycam004m-fake/cam4.bin ${D}${nonarch_base_libdir}/firmware/mycam004m-fake/cam4.bin
}

SYSTEMD_SERVICE:${PN} = "mycam004m-select-backend.service mycam004m-configure-pipeline.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

FILES:${PN} += "${bindir}/select-camera-backend.sh ${bindir}/mycam004m-select-backend.sh \
                 ${bindir}/mycam004m-configure-pipeline.sh \
                 ${systemd_unitdir}/system/mycam004m-select-backend.service \
                 ${systemd_unitdir}/system/mycam004m-configure-pipeline.service \
                 ${sysconfdir}/modules-load.d/mycam004m.conf \
                 ${sysconfdir}/modprobe.d/mycam004m-options.conf \
                 ${nonarch_base_libdir}/firmware/mycam004m-fake"

# The in-tree CSI stack the real backend needs (cdns-csi2rx comes in
# transitively via kernel-module-j721e-csi2rx's modinfo-derived RDEPENDS).
# These currently land on the image anyway, but modules-load.d/mycam004m.conf
# now force-loads them by name — make the dependency explicit so no future
# image trimming can turn that into a boot-time modprobe failure.
RDEPENDS:${PN} += "kernel-module-j721e-csi2rx kernel-module-cdns-dphy-rx"

# media-ctl (the split package from meta-oe's v4l-utils, no libv4l/Qt pull-in):
# mycam004m-configure-pipeline.sh needs it, and this minimal rootfs doesn't
# otherwise carry any v4l-utils.
RDEPENDS:${PN} += "media-ctl"
