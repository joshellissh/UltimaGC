IMAGE_INSTALL:append:beagleplay-ti = " ultima-app ultima-splash can-utils mmc-utils ultima-hwclock-load ultima-data-mount volatile-binds"

# WiFi AP (2026-08-18): hostapd for a standalone 5GHz AP on the onboard
# WL1807 (driver/hardware side unchanged, see ultima-wifi.cfg — AP mode is
# already exposed by mac80211/wlcore with no extra kernel config, same
# CFG80211/MAC80211/RFKILL/WLCORE/WLCORE_SDIO/WL18XX as STA used) + the
# WL18xx firmware blob it needs either way. Replaces the wpa-supplicant
# STA-to-Skynet setup from the same day (see git history) — WL1807 is a
# single-radio chip, confirmed dual-band != dual-radio (TI's own WL1837 E2E
# forum thread documents the throughput hit from time-slicing a single
# radio across channels in "multichannel" AP+STA mode), so this board being
# an AP and it being a Skynet client were never both available at once.
# AP-only was the explicit choice made over a runtime toggle between modes.
#
# hostapd is meta-oe (confirmed via `bitbake-layers show-recipes hostapd`
# against this build's actual layer set, not assumed present) — ships
# hostapd.service reading a single flat /etc/hostapd.conf (not a
# per-instance directory the way wpa_supplicant's config was), with
# SYSTEMD_AUTO_ENABLE "disable" at the whole-package level (confirmed by
# reading the recipe) — same granularity gap ultima_mask_timesyncd's
# comment already documents, so the wants-symlink is created directly
# below rather than fighting that variable.
#
# wl18xx-firmware is this layer's own recipe
# (recipes-kernel/wl18xx-firmware/), not oe-core's "linux-firmware-wl18xx" —
# that package builds empty in this environment (see wl18xx-firmware.bb's
# comment for what was actually confirmed: WHENCE and ${S} both have the
# file, but do_install's copy-firmware.sh drops it before ${D}, so no .ipk
# ever gets written and opkg fails with "No candidates to install").
#
# wireless-regdb-static, specifically — not plain wireless-regdb (meta,
# i.e. oe-core itself — confirmed via `bitbake-layers show-recipes
# wireless-regdb`). Its recipe (recipes-kernel/wireless-regdb in oe-core)
# splits PACKAGES = "${PN}-static ${PN}" and marks them RCONFLICTS: only
# -static's FILES include /lib/firmware/regulatory.db(.p7s), which cfg80211
# pulls in via the kernel's firmware-request path on kernel >=4.15 (this is
# 6.12, no crda daemon needed — that userspace-helper mechanism is the
# older path the plain, non-static package is actually for). First attempt
# here specified bare "wireless-regdb" — a validly-named package, so
# do_rootfs raised no error, but it doesn't ship regulatory.db at all
# (confirmed by inspecting the built rootfs: still empty; only caught by
# re-checking after the build claimed success, not by the build itself).
# Without regulatory.db the wiphy stays in the permissive "world"
# regulatory domain, which flags 5GHz no-initiating-radiation — a station
# can still associate on 5GHz there (it passively adopts the AP's country
# IE via 802.11d), which is why the prior STA-to-Skynet config never
# surfaced this, but hostapd trying to *beacon* on channel 36 would be
# rejected outright. Not yet confirmed on real hardware that this package
# alone is sufficient (i.e. that nothing else in this kernel's regulatory
# config blocks 5GHz AP mode) — only that regulatory.db now lands in the
# built rootfs, which it didn't before.
IMAGE_INSTALL:append:beagleplay-ti = " hostapd wireless-regdb-static wl18xx-firmware"

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
ROOTFS_POSTPROCESS_COMMAND += "ultima_mask_timesyncd; ultima_journald_volatile; ultima_coredump_disable; ultima_mask_resize_rootfs; ultima_mask_getty_tty1; ultima_enable_wifi_ap; "

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

# WiFi AP (2026-08-18): unlike the Skynet STA config it replaces, the SSID
# and passphrase are deliberately NOT baked into this file, or anywhere
# else in git — this network is one this board *hosts*, and (unlike
# Skynet, a fixed network this board only ever joins) its credentials
# shouldn't sit in repo history.
#
# First draft of this tried hostapd's config-file `include=` directive to
# pull the secret half in from /data at parse time — wrong, caught before
# it shipped: hostapd's config_file.c has no such directive at all (grepped
# the actual built source in the build volume; only C preprocessor
# #includes show up, no "include" config token). hostapd is also strict
# about unknown config lines (refuses to start on one), so that draft would
# have failed closed for the wrong reason — a config syntax error, not the
# "no credentials provisioned yet" case this is supposed to fail closed
# for. Real mechanism: ultima-hostapd-config.service (a oneshot) cats the
# static /etc/hostapd.conf together with /data/wifi-ap.conf into
# /run/hostapd-wlan0.conf at boot, and hostapd.service's ExecStart is
# overridden (via the .service.d drop-in below) to read that assembled
# file instead of /etc/hostapd.conf directly.
#
# /data/wifi-ap.conf is exactly two lines, plain hostapd config syntax —
# still not JSON, even though this is now a runtime assembly step rather
# than a native parse: it's a straight `cat`, not a real merge, so the
# secret file's syntax has to already match what the assembled file needs.
# A future JSON-based settings UI would need to emit these two lines in
# this syntax, not write raw JSON here:
#     ssid=<network name>
#     wpa_passphrase=<8-63 char WPA2 passphrase>
# Provisioning that file today is a manual one-time step over SSH/serial
# (see NOTES.md "WiFi AP") until a settings screen exists to write it from
# the touchscreen, the same way CalibrationSettingsScreen.qml writes
# calibration.json today. Fail-closed is still the design, just enforced a
# different way than the broken first draft assumed: the assembler's
# ExecStart explicitly test -f's /data/wifi-ap.conf first and exits
# non-zero if it's missing, writing nothing to /run — and because
# hostapd.service Requires= that unit below, a failed/skipped assembly run
# means hostapd never starts, rather than starting against a stale or
# partial /run file.
#
# hw_mode=g/channel=6 (2.4GHz), not the originally-planned 5GHz channel 36
# (UNII-1, chosen to dodge DFS's Channel Availability Check delay — still
# the right call if 5GHz ever works here). Downgraded after live hardware
# testing over SSH (wired-Ethernet session, wlan0 freed by stopping
# wpa_supplicant@wlan0.service first) rejected channel 36 outright:
# hostapd's journal showed "IEEE 802.11 Configured channel (36) or
# frequency (5180) ... not found from the channel list of the current mode
# (2) IEEE 802.11a" / "Hardware does not support configured channel". Root
# cause visible in dmesg from first boot, unrelated to this change: wlcore
# logs "could not get configuration binary ti-connectivity/wl18xx-conf.bin:
# -2" and "WARNING falling back to default config" — wl18xx-firmware (this
# layer's recipe) only ever vendored the one firmware blob the driver's
# base probe needs (see that recipe's own comment), not the NVS
# calibration file (wl1271-nvs.bin) or this conf.bin, and the driver's
# fallback default config apparently doesn't expose the 802.11a (5GHz)
# channel list at all. Not yet root-caused further than that — unconfirmed
# whether adding those two firmware files would unlock 5GHz outright, or
# whether NVS calibration data is board/antenna-specific and can't just be
# vendored generically the way the base firmware blob was. Real follow-up
# item, not attempted here. hw_mode=g still runs `ieee80211n=1` (802.11n
# on 2.4GHz is unaffected by any of this) and `ieee80211d=1` (country IE
# advertisement, harmless/good-practice regardless of band) — dropped
# `ieee80211h=1` since it's DFS/TPC-specific to 5GHz and doesn't apply
# here. `wireless-regdb-static` (see IMAGE_INSTALL comment above) stays in
# the image either way: harmless for 2.4GHz-only operation, and already in
# place for whenever 5GHz gets unblocked.
#
# Hardware-verified end to end on real hardware over the wired-Ethernet SSH
# path (192.168.50.220, not the beagleplay-ti.local mDNS name, specifically
# so stopping wpa_supplicant@wlan0 couldn't cut the session): after
# installing the built binary/config/units by hand (root remounted rw only
# for the copy, back to ro immediately after — all the shared libs hostapd
# needs, libnl-3/libnl-genl-3/libssl/libcrypto, were already present on the
# board from other packages, nothing else needed) and writing the real
# `ssid=Ultima RS` / `wpa_passphrase=linkedlist` to /data/wifi-ap.conf,
# hostapd.service went active (not failed/deactivated), wlan0 came up at
# 192.168.4.1/24 exactly as configured, and dmesg showed a clean
# `deauthenticating ... by local choice (Reason: 3=DEAUTH_LEAVING)` from
# the old Skynet BSSID as the interface transitioned from STA to AP — no
# crash, no instability. Also removed the original image's
# wpa_supplicant@wlan0.service wants-symlink from the live board (it was
# still `WantedBy=multi-user.target` from the prior STA build and would
# have raced hostapd for wlan0 on the next reboot otherwise) — confirmed
# `wpa_supplicant@wlan0.service` now inactive with no wants-symlink,
# `hostapd.service` active, root back to `ro,relatime`. Not yet confirmed:
# an actual phone associating (only the AP side was checked; macOS's own
# WiFi-scan tools — `airport -s`, `system_profiler SPAirPortDataType` —
# came back empty on this Mac, inconclusive rather than a negative signal,
# most likely a Location Services/permission restriction on the deprecated
# `airport` tool rather than anything about the AP itself).
#
# hostapd.service ships with only After=network.target (confirmed by
# reading the built unit) — no awareness of wlan0, /data, or the assembled
# config path, unlike wpa_supplicant@.service's baked-in wlan0 ordering the
# Skynet setup relied on. The .service.d drop-in adds all of that
# explicitly: After=/Requires= on the wlan0 device (module autoload from
# the SDIO device ID brings it up, same as before, no explicit modprobe
# needed) and on ultima-hostapd-config.service, plus an ExecStart=
# override (blank line first to clear the upstream one, standard systemd
# drop-in mechanics) pointing at /run/hostapd-wlan0.conf instead of
# /etc/hostapd.conf.
#
# 10-wlan-ap.network sorts ahead of the base tisdk image's own
# /etc/systemd/network/30-wlan.network (Name=wlan*, DHCP=yes — confirmed by
# inspecting the actual built rootfs, not just the meta-arago-distro recipe
# source path, which is a different, inactive location) — both land in the
# same directory, so this is a plain lexical-sort win, no cross-directory
# (/etc vs /run vs /usr/lib) precedence question to reason about at all.
# systemd-networkd applies only the first matching .network file per
# interface, so DHCP=yes never applies to wlan0. DHCPServer=yes is
# systemd-networkd's own DHCP server implementation (this build's systemd
# is v255, comfortably new enough) — no dnsmasq/udhcpd package needed on
# top of hostapd.
#
# Build-verified AND hardware-verified (see the hw_mode=g comment above for
# the full story — channel 36/5GHz was hardware-verified to fail, this
# 2.4GHz config was hardware-verified to work). `./build.sh
# tisdk-base-image` ran clean and the produced rootfs was inspected
# directly — every file below landed exactly as written, no wpa_supplicant
# leftovers, hostapd binary present, regulatory.db present. Still not
# verified: an actual phone associating (only the AP side was checked from
# this Mac — see above).
ultima_enable_wifi_ap () {
    cat > ${IMAGE_ROOTFS}${sysconfdir}/hostapd.conf <<'EOF'
interface=wlan0
driver=nl80211
hw_mode=g
channel=6
ieee80211d=1
country_code=US
ieee80211n=1
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
auth_algs=1
wmm_enabled=1
EOF

    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/network
    cat > ${IMAGE_ROOTFS}${sysconfdir}/systemd/network/10-wlan-ap.network <<'EOF'
[Match]
Name=wlan0

[Network]
Address=192.168.4.1/24
DHCPServer=yes
EOF

    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system
    cat > ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/ultima-hostapd-config.service <<'EOF'
[Unit]
Description=Assemble /run/hostapd-wlan0.conf from the static base config plus /data/wifi-ap.conf
After=ultima-data-mount.service
Requires=ultima-data-mount.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'test -f /data/wifi-ap.conf && cat /etc/hostapd.conf /data/wifi-ap.conf > /run/hostapd-wlan0.conf'
EOF

    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/hostapd.service.d
    cat > ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/hostapd.service.d/ultima.conf <<'EOF'
[Unit]
After=sys-subsystem-net-devices-wlan0.device ultima-hostapd-config.service
Requires=sys-subsystem-net-devices-wlan0.device ultima-hostapd-config.service

[Service]
ExecStart=
ExecStart=/usr/sbin/hostapd /run/hostapd-wlan0.conf -P /run/hostapd.pid -B
EOF

    install -d ${IMAGE_ROOTFS}${systemd_system_unitdir}/multi-user.target.wants
    ln -sf ${systemd_system_unitdir}/hostapd.service \
        ${IMAGE_ROOTFS}${systemd_system_unitdir}/multi-user.target.wants/hostapd.service
}
