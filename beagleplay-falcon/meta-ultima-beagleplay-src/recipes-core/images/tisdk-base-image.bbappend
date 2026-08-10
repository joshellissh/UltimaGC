IMAGE_INSTALL:append:beagleplay-ti = " ultima-app can-utils mmc-utils ultima-hwclock-load"

# Disable systemd-timesyncd — it silently overwrites whatever SetTimeScreen
# just wrote via SystemClock::setTime() any time the board has network (see
# NOTES.md "Dash clock doesn't persist a manual set"). First attempt at this
# was SYSTEMD_AUTO_ENABLE:pn-systemd-timesyncd = "disable" (see git history
# of recipes-ultima/boot-trim) — confirmed a no-op on real hardware
# (is-enabled still showed "enabled" after that build). Root cause:
# systemd-timesyncd.service ships inside the base "systemd" package in this
# build, not split into its own systemd-timesyncd sub-package, and
# SYSTEMD_AUTO_ENABLE only resolves at whole-package granularity (see
# systemd.bbclass's systemd_populate_packages) — there's no package named
# "systemd-timesyncd" for that override to attach to. A package-level
# override on "systemd" itself would be far too broad: that package's
# SYSTEMD_SERVICE list covers more than just timesyncd. Masking the unit
# directly in the finished rootfs is the standard, surgical way to disable
# exactly one service regardless of which package owns it — this runs after
# all package postinsts (including systemd's own "enable" pass), so it wins
# regardless of ordering.
ROOTFS_POSTPROCESS_COMMAND += "ultima_mask_timesyncd; "

ultima_mask_timesyncd () {
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/multi-user.target.wants/systemd-timesyncd.service
    ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/systemd-timesyncd.service
}
