SUMMARY = "Auto-mount/unmount the dashcam recording USB drive by filesystem LABEL"
DESCRIPTION = "Hotplug udev rule (files/99-ultima-dvr.rules): mounts any block \
device labeled ULTIMA_DVR to /mnt/dvr via systemd-mount, unmounts on removal. \
An unlabeled/differently-labeled drive is never auto-formatted -- instead \
ultima-app offers the driver an on-screen 'format this drive?' dialog, which \
runs ultima-dvr-format (files/ultima-dvr-format.sh) on confirmation. LABEL is \
unambiguous here, unlike ultima-data-mount.sh's /data (every SD card dd'd from \
this project's own image shares one UUID/LABEL) -- this is the only removable \
disk on the system, so a drive's own LABEL genuinely identifies it."
LICENSE = "CLOSED"

COMPATIBLE_MACHINE = "beagley-ai"

SRC_URI = "file://99-ultima-dvr.rules file://ultima-dvr-mount.sh file://ultima-dvr-format.sh"
S = "${WORKDIR}"

# exfatprogs (meta-openembedded/meta-filesystems): fsck.exfat for the
# preen-before-mount step in ultima-dvr-mount.sh (exFAT has no journal and this
# drive is power-cut constantly, DASHCAM.md M3) AND mkfs.exfat for the
# on-confirmation format in ultima-dvr-format.sh. util-linux-blockdev provides
# `blockdev --rereadpt` (drop stale partition nodes after a whole-disk mkfs).
# systemd-run/systemd-mount/udevadm come from systemd, always present.
RDEPENDS:${PN} = "exfatprogs util-linux-blockdev"

do_install() {
    install -d ${D}${sysconfdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/99-ultima-dvr.rules ${D}${sysconfdir}/udev/rules.d/99-ultima-dvr.rules

    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/ultima-dvr-mount.sh ${D}${bindir}/ultima-dvr-mount
    # The destructive format helper the app's dialog invokes; own safety guards.
    install -m 0755 ${WORKDIR}/ultima-dvr-format.sh ${D}${bindir}/ultima-dvr-format

    # Baked into the rootfs at build time, same reasoning as ultima-app.bb's
    # /data mountpoint: systemd-mount (triggered by the udev rule above)
    # mounts onto this, it doesn't create it.
    install -d ${D}/mnt/dvr
}

FILES:${PN} += "${sysconfdir}/udev/rules.d/99-ultima-dvr.rules ${bindir}/ultima-dvr-mount ${bindir}/ultima-dvr-format /mnt/dvr"
