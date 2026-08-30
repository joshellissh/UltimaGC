# Recipe defaults this to "enable" itself. See docker-moby_%.bbappend in
# this same directory for why this is a disable, not a PACKAGE_EXCLUDE.
# Telnet with no auth on a car's onboard network is also just not something
# worth shipping enabled by default — SSH (dropbear) already covers remote
# access.
SYSTEMD_AUTO_ENABLE:${PN} = "disable"
