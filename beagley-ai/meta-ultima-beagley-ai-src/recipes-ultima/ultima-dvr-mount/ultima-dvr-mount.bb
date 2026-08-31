SUMMARY = "Auto-mount/unmount the dashcam recording USB drive by filesystem LABEL"
DESCRIPTION = "Hotplug udev rule (files/99-ultima-dvr.rules): mounts any block \
device labeled ULTIMA_DVR to /mnt/dvr via systemd-mount, unmounts on removal. \
Deliberately does nothing for an unlabeled/differently-labeled drive -- no \
auto-format. LABEL is unambiguous here, unlike ultima-data-mount.sh's /data \
(every SD card dd'd from this project's own image shares one UUID/LABEL) -- \
this is the only removable disk on the system, so a drive's own LABEL \
genuinely identifies it. Format the drive yourself first: exFAT, LABEL=ULTIMA_DVR."
LICENSE = "CLOSED"

COMPATIBLE_MACHINE = "beagley-ai"

SRC_URI = "file://99-ultima-dvr.rules"
S = "${WORKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/99-ultima-dvr.rules ${D}${sysconfdir}/udev/rules.d/99-ultima-dvr.rules

    # Baked into the rootfs at build time, same reasoning as ultima-app.bb's
    # /data mountpoint: systemd-mount (triggered by the udev rule above)
    # mounts onto this, it doesn't create it.
    install -d ${D}/mnt/dvr
}

FILES:${PN} += "${sysconfdir}/udev/rules.d/99-ultima-dvr.rules /mnt/dvr"
