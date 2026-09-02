SUMMARY = "Boot-time media-ctl format propagation for the NVP6324 CSI-2 pipeline"
DESCRIPTION = "The NVP6324 subdev sources UYVY 1920x1080, but the downstream \
Cadence CSI2RX bridge and TI CSI2RX SHIM pads come up at their 640x480 \
default. V4L2 link validation rejects the mismatch at STREAMON with -EPIPE, so \
a plain capture on /dev/video2 fails even though the chip is locked and \
streaming. This oneshot pushes the format down the pipeline once at boot (it \
then persists across STREAMOFF/STREAMON), so any consumer — camqa today, \
ultima-app later — can just open the node and stream. The media entity names \
are SoC/DT-specific, which is why this lives in the board layer rather than \
the board-agnostic ultima-app."
LICENSE = "CLOSED"

inherit systemd

SRC_URI = "file://nvp6324-csi-setup.service file://nvp6324-csi-setup.sh"

SYSTEMD_SERVICE:${PN} = "nvp6324-csi-setup.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

# media-ctl (v4l-utils) is the only runtime dependency; it is already pulled
# into the image alongside the driver (see tisdk-base-image.bbappend), but
# state it so this package is self-consistent.
RDEPENDS:${PN} = "v4l-utils"

do_install() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/nvp6324-csi-setup.service ${D}${systemd_unitdir}/system/nvp6324-csi-setup.service

    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/nvp6324-csi-setup.sh ${D}${bindir}/nvp6324-csi-setup.sh
}

FILES:${PN} += "${systemd_unitdir}/system/nvp6324-csi-setup.service ${bindir}/nvp6324-csi-setup.sh"
