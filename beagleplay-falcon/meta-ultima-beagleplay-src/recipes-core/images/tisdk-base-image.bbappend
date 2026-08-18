IMAGE_INSTALL:append:beagleplay-ti = " ultima-app ultima-splash can-utils mmc-utils ultima-hwclock-load ultima-data-mount volatile-binds"

# WiFi (2026-08-18): wpa-supplicant for WPA2-PSK association + the WL18xx
# firmware blob the onboard WL1807 needs (see ultima-wifi.cfg for the
# driver/hardware side). systemd-networkd already ships a generic
# /usr/lib/systemd/network/30-wlan.network (Name=wlan*, DHCP=yes) in the base
# tisdk image — confirmed present on real hardware even with WiFi disabled,
# so no new .network file is needed here, only the WPA layer.
#
# wl18xx-firmware is this layer's own recipe
# (recipes-kernel/wl18xx-firmware/), not oe-core's "linux-firmware-wl18xx" —
# that package builds empty in this environment (see wl18xx-firmware.bb's
# comment for what was actually confirmed: WHENCE and ${S} both have the
# file, but do_install's copy-firmware.sh drops it before ${D}, so no .ipk
# ever gets written and opkg fails with "No candidates to install").
IMAGE_INSTALL:append:beagleplay-ti = " wpa-supplicant wl18xx-firmware"

# mycam004m (2026-08-17): the quad-camera V4L2 driver's mock/fake backend
# (mycam004m-fake.ko + its 4 static reference frames + the boot-time
# select-camera-backend.sh run), enabled by default since no real
# MY-CAM004M hardware is attached yet — see
# recipes-kernel/mycam004m/mycam004m.bb. The real backend's .ko is built
# by the same recipe but its devicetree overlay is deliberately not
# applied here, so it stays inert.
IMAGE_INSTALL:append:beagleplay-ti = " mycam004m"

# GPU enablement spike (2026-08-12): explicit rather than relying on the
# RRECOMMENDS chain (mesa-megadriver -> ti-img-rogue-driver ->
# ti-img-rogue-umlibs, see mesa-pvr_24.0.1.bb) to pull the rogue stack in on
# its own. kmscube/mesa-demos are smoke-test tools only for verifying the
# GPU works before ultima-app is blamed for anything — remove both once the
# spike is confirmed working, they don't belong in a shipped image.
IMAGE_INSTALL:append:beagleplay-ti = " ti-img-rogue-driver ti-img-rogue-umlibs kmscube mesa-demos"

# Adds a small ext4 /data partition (p3) after the stock boot+root layout —
# see wic/ultima-beagleplay.wks.in for the full reasoning, including a corrected
# mistake: p1 (vfat, --source bootimg-efi) holds tiboot3.bin and is required
# for SD boot, confirmed by hardware test after an earlier version of this
# file repurposed it and left the board with no LEDs/no video (the ROM had
# nothing to load). WKS_SEARCH_PATH already includes this layer's wic/ dir by
# convention (confirmed via `bitbake -e tisdk-base-image`), so the bare
# filename resolves.
WKS_FILE:beagleplay-ti = "ultima-beagleplay.wks.in"

# read-only-rootfs (2026-08-10): root (p2) becomes mount-time read-only.
# oe-core's rootfs-postcommands.bbclass handles most of this automatically
# once this feature is on — confirmed by reading it rather than assuming:
# it appends "ro" to the kernel cmdline itself, rewrites /etc/fstab's
# "/dev/root" line to ro (matches this image's fstab as shipped, no
# wic/ultima-beagleplay.wks.in change needed), empties /etc/machine-id at
# build time so systemd's transient-ID support takes over, and redirects
# dropbear to generate its host key under /var/lib/dropbear instead of
# /etc/dropbear once it sees no key baked into the image. The one thing that
# mechanism assumes and this distro doesn't provide on its own is somewhere
# writable for that redirected state to actually land — volatile-binds
# (stock oe-core, not present in any TI/arago layer by default) bind-mounts
# tmpfs over /var/lib, /var/cache, /var/spool and /srv for exactly that.
# Consequence accepted rather than engineered around: both machine-id and
# the dropbear host key become transient, regenerated fresh every boot,
# since nothing here persists /var/lib across reboots. That matches this
# project's already-documented tolerance for dropbear host-key churn (see
# emmc-push.sh's SSH_OPTS comment) — not treated as a gap worth the added
# complexity of persisting either one onto /data.
#
# Checked directly against this board rather than assumed clean: /tmp is
# already tmpfs via systemd's static tmp.mount (ultima-app.service's
# ExecStartPre mkdir is unaffected), and ultima-hwclock-load's
# `hwclock --hctosys` does not write /etc/adjtime on this build (confirmed
# absent after 5+ minutes of uptime) — no /etc write-path risk from either.
#
# Not yet verified: can-utils/mmc-utils/ultima-* and the boot-trim-disabled
# but still-installed docker-moby/containerd/netperf/lldpd/psplash all need
# do_rootfs to actually succeed with this feature on — any postinst that
# assumes it can defer work to first boot fails the build outright under
# read-only-rootfs, which is the intended discovery mechanism (same
# approach that surfaced the WKS_FILE ".wks" vs ".wks.in" mistake above:
# let a real build failure name the exact problem instead of guessing).
IMAGE_FEATURES += "read-only-rootfs"

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
ROOTFS_POSTPROCESS_COMMAND += "ultima_mask_timesyncd; ultima_journald_volatile; ultima_coredump_disable; ultima_mask_resize_rootfs; ultima_mask_getty_tty1; ultima_enable_wifi; "

ultima_mask_timesyncd () {
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/multi-user.target.wants/systemd-timesyncd.service
    ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/systemd-timesyncd.service
}

# meta-ti-foundational/recipes-ti/resize-rootfs's resize_rootfs.service grows
# partition 2 (hardcoded `sfdisk -N 2 ... echo ",+"`) into whatever free space
# follows it on first boot. Now that wic/ultima-beagleplay.wks.in adds /data as a
# real partition 3 after root, that free-space grab would race /data for the
# same space. Masking it removes the collision outright, and it's a wanted
# side effect regardless: a live sfdisk+resize2fs against the same disk Falcon
# boots straight off of is a boot-availability risk this project doesn't want,
# and a bounded root size is the actual goal of the read-only-rootfs port
# this is groundwork for.
ultima_mask_resize_rootfs () {
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/multi-user.target.wants/resize_rootfs.service
    ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/resize_rootfs.service
}

# Groundwork for a read-only-rootfs port (see NOTES.md "First hardware
# attempt: the crash-loop and its risk"): a dozens-of-restarts crash-loop
# produced real I/O errors on the SD card's rootfs partition, and journald +
# likely systemd-coredump flushing to /var on every restart was the
# documented cause. Storage=volatile keeps the journal on tmpfs (/run) and
# never flushes to disk, independent of whether root ever becomes read-only —
# this alone removes the actual culprit from that incident.
ultima_journald_volatile () {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/journald.conf.d
    printf '[Journal]\nStorage=volatile\n' > ${IMAGE_ROOTFS}${sysconfdir}/systemd/journald.conf.d/ultima-volatile.conf
}

# Companion to ultima_journald_volatile above — systemd-coredump was the
# other suspected (not confirmed) writer in the same incident. Storage=none
# drops core dumps instead of writing them to /var/lib/systemd/coredump. This
# is a harmless no-op if systemd-coredump isn't actually installed in this
# image; kept anyway so it isn't a gap if that changes later.
ultima_coredump_disable () {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/coredump.conf.d
    printf '[Coredump]\nStorage=none\n' > ${IMAGE_ROOTFS}${sysconfdir}/systemd/coredump.conf.d/ultima-disable.conf
}

# The 2026-08-11 boot-splash investigation (see NOTES.md) found
# getty@tty1.service alive on tty1 -- the same VT fbcon and any fbdev
# splash draw into -- writing a login prompt over whatever's on screen
# shortly after boot. That was only masked live/temporarily during that
# investigation and reverted afterward. ultima-splash.bb makes that
# permanent: a login prompt agetty never intended to be seen (this board's
# only interactive access is serial -- see serial-getty@, a separate
# template unit this doesn't touch -- or SSH) would otherwise overwrite the
# splash pixels within the first couple seconds of boot. Same
# symlink-to-/dev/null mask pattern as ultima_mask_timesyncd above --
# `systemctl mask` wins regardless of how the unit would otherwise be
# pulled in (getty.target's static Alias=, in this case), so no
# wants-symlink removal is needed alongside it.
ultima_mask_getty_tty1 () {
    ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/getty@tty1.service
}

# WiFi (2026-08-18): bakes the Skynet credentials directly into the image
# rather than sourcing them from /data at runtime — this is one fixed known
# network, not a network that needs to change without a rebuild, so the
# /data-persistence pattern odometer.json uses would be unjustified
# complexity here. SSID is case-sensitive — confirmed against a live scan on
# real hardware that the broadcast name is "Skynet", not "skynet" (a lowercase
# name was the initial ask, would have silently never associated). wpa-supplicant's own recipe installs
# wpa_supplicant@.service (systemd/wpa_supplicant@.service upstream,
# confirmed by reading the built package rather than assuming) with
# ExecStart=wpa_supplicant -c/etc/wpa_supplicant/wpa_supplicant-%I.conf
# -i%I — so the instance name IS the config filename suffix, must be wlan0
# exactly. That unit's [Install] is WantedBy=multi-user.target but
# SYSTEMD_AUTO_ENABLE is "disable" at the whole-recipe level (covers every
# package/unit this recipe ships, including the plain wpa_supplicant.service
# meant for DBus/NetworkManager use that this image doesn't run) — same
# whole-package-granularity problem ultima_mask_timesyncd's comment already
# documents for systemd-timesyncd, so this creates the wants-symlink
# directly rather than fighting SYSTEMD_AUTO_ENABLE per-instance.
# Requires=/After=sys-subsystem-net-devices-wlan0.device in that unit means
# it only starts once the wl18xx driver brings wlan0 up — no explicit
# modprobe step needed, module autoload from the SDIO device ID handles
# that, same as every other =m driver in this image.
ultima_enable_wifi () {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/wpa_supplicant
    cat > ${IMAGE_ROOTFS}${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf <<'EOF'
ctrl_interface=/var/run/wpa_supplicant
update_config=0

network={
    ssid="Skynet"
    psk="linkedlist"
    }
EOF
    chmod 600 ${IMAGE_ROOTFS}${sysconfdir}/wpa_supplicant/wpa_supplicant-wlan0.conf

    install -d ${IMAGE_ROOTFS}${systemd_system_unitdir}/multi-user.target.wants
    ln -sf ${systemd_system_unitdir}/wpa_supplicant@.service \
        ${IMAGE_ROOTFS}${systemd_system_unitdir}/multi-user.target.wants/wpa_supplicant@wlan0.service
}
