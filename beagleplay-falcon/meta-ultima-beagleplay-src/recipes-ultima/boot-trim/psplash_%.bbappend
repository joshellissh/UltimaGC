# psplash-start.service fails on every single boot on this image (confirmed
# in captured boot logs — "[FAILED] Failed to start Start psplash boot
# splash screen", cascading into a dependency-failed line for
# psplash-systemd.service right after) — there's no framebuffer for it to
# draw a splash to until tidss probes, so it's pure permanent failure, not a
# race. See docker-moby_%.bbappend in this same directory for why this is a
# disable, not a PACKAGE_EXCLUDE.
SYSTEMD_AUTO_ENABLE:${PN} = "disable"
