# See docker-moby_%.bbappend in this same directory for why this is a
# disable, not a PACKAGE_EXCLUDE.
SYSTEMD_AUTO_ENABLE:${PN} = "disable"
