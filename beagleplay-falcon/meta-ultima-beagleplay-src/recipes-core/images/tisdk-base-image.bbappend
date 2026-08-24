IMAGE_INSTALL:append:beagleplay-ti = " ultima-app ultima-splash can-utils mmc-utils ultima-hwclock-load ultima-data-mount volatile-binds"

# WiFi client mode (2026-08-19): reverted from the standalone-AP +
# captive-portal setup (see git history and NOTES.md "WiFi AP" /
# "Captive portal + phone settings UI" for the full story of what was
# tried and why it was abandoned — real-device testing showed the
# OS-captive-portal-probe dance too unreliable to build on; a Bluetooth
# link is the new plan for the phone-facing side instead). Back to
# wpa_supplicant STA-to-Skynet, same driver/hardware side either way (see
# ultima-wifi.cfg) — the WL1807's single-radio limitation that originally
# motivated dropping STA-and-AP-simultaneously is moot now that AP mode
# itself is gone.
#
# Unlike the very first STA-to-Skynet attempt (see git history — baked
# ssid/psk straight into this file), the network config is assembled at
# boot from a static base plus /data/wifi-client.conf (not committed to
# git), same pattern the AP work built for its own credentials and for the
# same reason: a home WiFi password doesn't belong in repo history.
# Provisioning that file is a manual one-time step over SSH — see
# ultima_enable_wifi's own comment below for the exact mechanism.
IMAGE_INSTALL:append:beagleplay-ti = " wpa-supplicant wl18xx-firmware"

# mycam004m (2026-08-17, real backend wired up 2026-08-23): the
# quad-camera V4L2 driver, both backends built. Real (mycam004m.ko +
# TI/Cadence's in-tree CSI2/D-PHY modules, camera devicetree node applied
# via a compile-time patch to k3-am625-beagleplay.dts — see
# recipes-kernel/linux/linux-ti-staging/0002-...-add-mycam004m-camera.patch)
# is now the boot default now that MY-CAM004M hardware is actually
# attached; mycam004m-fake stays built and force-loaded alongside it as a
# live fallback (select-camera-backend.sh fake) and the thing to keep
# developing app logic against regardless of real's hardware-verification
# status. See recipes-kernel/mycam004m/mycam004m.bb and NOTES.md.
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

# WiFi client mode (2026-08-19): STA-to-Skynet, same overall shape as the
# very first WiFi commit (see git history) but with the credential handling
# the AP work's own comment already argued for — a fixed home network's
# password still doesn't belong baked into a git-tracked file, so this
# assembles /run/wpa_supplicant-wlan0.conf at boot from a static base plus
# /data/wifi-client.conf, exact same "cat base + secret = assembled config"
# mechanism ultima-hostapd-config.service already used for the AP's own
# passphrase — reused deliberately rather than inventing a second pattern.
#
# /data/wifi-client.conf is the network={ ssid=... psk=... } stanza only,
# plain wpa_supplicant config syntax:
#     network={
#         ssid="<network name>"
#         psk="<WPA2 passphrase, 8-63 chars>"
#     }
# Provisioning it is a manual one-time step over SSH:
#     ssh root@<board-ip> 'cat > /data/wifi-client.conf' <<'EOF'
#     network={
#         ssid="Skynet"
#         psk="..."
#     }
#     EOF
#     systemctl restart ultima-wpa-supplicant-config wpa_supplicant@wlan0
# Fail-closed, same as the AP config assembler: the ExecStart below test
# -f's /data/wifi-client.conf first and writes nothing to /run if it's
# missing, and the wpa_supplicant@wlan0.service.d drop-in's Requires= on
# the assembler means a missing/failed assembly leaves WiFi simply not
# started, not started against a stale or partial file.
#
# wpa_supplicant@.service ships with Requires=/After=
# sys-subsystem-net-devices-%i.device baked into the upstream unit itself
# (confirmed by reading the built package — unlike hostapd.service, which
# needed that ordering added via a drop-in) — the instance-specific
# .service.d drop-in below only needs to add the assembler dependency and
# override ExecStart to point at the assembled /run file instead of
# /etc/wpa_supplicant/wpa_supplicant-wlan0.conf directly.
#
# No custom .network file needed for STA mode — the base tisdk image's own
# /usr/lib/systemd/system... equivalent /etc/systemd/network/30-wlan.network
# (Name=wlan*, DHCP=yes) already handles DHCP client mode for wlan0 (this
# is exactly why the AP work needed its own 10-wlan-ap.network to sort
# ahead of it, and exactly why removing that file here is correct, not an
# oversight).

ultima_enable_wifi () {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/wpa_supplicant
    cat > ${IMAGE_ROOTFS}${sysconfdir}/wpa_supplicant/wpa_supplicant-base.conf <<'EOF'
ctrl_interface=/var/run/wpa_supplicant
update_config=0
EOF

    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system
    cat > ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/ultima-wpa-supplicant-config.service <<'EOF'
[Unit]
Description=Assemble /run/wpa_supplicant-wlan0.conf from the static base config plus /data/wifi-client.conf
After=ultima-data-mount.service
Requires=ultima-data-mount.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'test -f /data/wifi-client.conf && cat /etc/wpa_supplicant/wpa_supplicant-base.conf /data/wifi-client.conf > /run/wpa_supplicant-wlan0.conf'
EOF

    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/wpa_supplicant@wlan0.service.d
    cat > ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/wpa_supplicant@wlan0.service.d/ultima.conf <<'EOF'
[Unit]
After=ultima-wpa-supplicant-config.service
Requires=ultima-wpa-supplicant-config.service

[Service]
ExecStart=
ExecStart=/usr/sbin/wpa_supplicant -c/run/wpa_supplicant-wlan0.conf -iwlan0
EOF

    install -d ${IMAGE_ROOTFS}${systemd_system_unitdir}/multi-user.target.wants
    ln -sf ${systemd_system_unitdir}/wpa_supplicant@.service \
        ${IMAGE_ROOTFS}${systemd_system_unitdir}/multi-user.target.wants/wpa_supplicant@wlan0.service
}
