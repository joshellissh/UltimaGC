SUMMARY = "TI WL18xx (WiLink8) WiFi firmware for the onboard WL1807"
DESCRIPTION = "Just the one firmware file wlcore/wl18xx actually requests on \
this board (confirmed against real boot logs: 'wlcore: ERROR could not get \
firmware ti-connectivity/wl18xx-fw-4.bin'), vendored directly rather than \
pulled from oe-core's linux-firmware recipe."

# oe-core's linux-firmware_20240909.bb lists this exact file under its
# ${PN}-wl18xx split package (FILES:${PN}-wl18xx = ".../ti-connectivity/wl18*",
# WHENCE correctly lists all 35 ti-connectivity/ files including this one, and
# the file is genuinely present in that recipe's ${S} after fetch/unpack) —
# but building linux-firmware in this environment produces an EMPTY
# ${PN}-wl18xx package: nothing under ti-connectivity/ makes it into ${D}, so
# no .ipk is ever written for it, and installing "linux-firmware-wl18xx" into
# an image fails with "opkg_solver_install: No candidates". Confirmed by
# inspecting the built recipe's packages-split/ directly (empty) rather than
# guessing — WHENCE and the source files are both fine, so the gap is in
# do_install's copy-firmware.sh (WHENCE-driven, shells out to rdfind for
# dedup) somewhere between "file exists in ${S}" and "file lands in ${D}".
# Not root-caused further than that; not worth blocking WiFi bring-up on a
# pre-existing oe-core/environment bug when the fix is one file. If this
# environment's linux-firmware build ever gets fixed, this recipe can be
# dropped in favor of "linux-firmware-wl18xx" in IMAGE_INSTALL.
LICENSE = "Firmware-ti-connectivity"
LIC_FILES_CHKSUM = "file://LICENCE.ti-connectivity;md5=3b1e9cf54aba8146dad4b735777d406f"
NO_GENERIC_LICENSE[Firmware-ti-connectivity] = "LICENCE.ti-connectivity"

SRC_URI = "file://wl18xx-fw-4.bin file://LICENCE.ti-connectivity"

S = "${WORKDIR}"

inherit allarch

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware/ti-connectivity
    install -m 0644 ${WORKDIR}/wl18xx-fw-4.bin ${D}${nonarch_base_libdir}/firmware/ti-connectivity/
    install -d ${D}${nonarch_base_libdir}/firmware
    install -m 0644 ${WORKDIR}/LICENCE.ti-connectivity ${D}${nonarch_base_libdir}/firmware/
}

FILES:${PN} = " \
    ${nonarch_base_libdir}/firmware/ti-connectivity/wl18xx-fw-4.bin \
    ${nonarch_base_libdir}/firmware/LICENCE.ti-connectivity \
"
