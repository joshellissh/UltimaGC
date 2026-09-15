SUMMARY = "Bring up the onboard cc33xx WiFi radio and connect it to the bench network"
DESCRIPTION = "BeagleY-AI's onboard TI cc33xx radio needs firmware plus a \
wpa_supplicant instance pointed at a network config to associate -- DHCP \
itself is already handled by Arago's stock 30-wlan.network (Name=wlan*, \
DHCP=yes). RDEPENDS pulls in the cc33xx firmware/target-scripts/conf tool \
alongside wpa-supplicant so installing this one recipe is enough."
LICENSE = "CLOSED"

inherit systemd

SRC_URI = "file://ultima-wifi-connect.service file://wpa_supplicant-wlan0.conf"

# cc33xx-fw-tilinux (not meta-ti's stock cc33xx-fw) supplies the firmware: the
# stock package's cc33xx-conf.bin version-mismatches this board's kernel driver
# and the FW download fails at boot. See recipes-bsp/cc33xx-fw-tilinux.
RDEPENDS:${PN} = "wpa-supplicant cc33xx-fw-tilinux cc33xx-target-scripts cc33conf"

SYSTEMD_SERVICE:${PN} = "ultima-wifi-connect.service"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install() {
    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${WORKDIR}/ultima-wifi-connect.service ${D}${systemd_unitdir}/system/ultima-wifi-connect.service

    install -d ${D}${sysconfdir}
    install -m 0600 ${WORKDIR}/wpa_supplicant-wlan0.conf ${D}${sysconfdir}/wpa_supplicant-wlan0.conf
}

FILES:${PN} += "${systemd_unitdir}/system/ultima-wifi-connect.service ${sysconfdir}/wpa_supplicant-wlan0.conf"
CONFFILES:${PN} += "${sysconfdir}/wpa_supplicant-wlan0.conf"
