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

# Bluetooth (2026-08-19, status: BLOCKED — see NOTES.md "Bluetooth via
# CC1352P7" for the full story). BeaglePlay's WL1807 is WiFi-only (the
# WiLink8 "combo" parts with BT are the WL183x line, not WL1807; confirmed
# against the actual dts, no BT UART node exists for it), so the plan was
# Bluetooth via the onboard CC1352P7 wireless MCU (normally used for
# Greybus/BeagleConnect over the same UART) flashed with TI BLE host_test
# firmware, attached via bluez5 + btattach. That plan is dead: host_test is
# a "network processor" build with its own Host (GAP/GATT/SMP) on-chip, and
# TI's own docs say plainly it's "not possible to support an external BLE
# Host" against it — BlueZ can't drive it no matter how the UART framing is
# handled (see the comment further down where ultima_enable_bluetooth()
# used to be for the full explanation). bluez5 is left installed regardless —
# still needed whatever ends up feeding hci0 (a USB dongle needs it exactly
# as much as a real controller-only CC1352P7 firmware would), and its
# default PACKAGECONFIG already includes tools (btattach/hcitool) and
# systemd (bluetooth.service). It DOES need an override now — see
# ultima_bluetooth_persist_bonds() below, added once BLE actually started
# working via the CC1352P7-as-controller path (see NOTES.md, same section,
# "Hardware-verified working").
IMAGE_INSTALL:append:beagleplay-ti = " bluez5"

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
ROOTFS_POSTPROCESS_COMMAND += "ultima_mask_timesyncd; ultima_journald_volatile; ultima_coredump_disable; ultima_mask_resize_rootfs; ultima_mask_getty_tty1; ultima_enable_wifi; ultima_bluetooth_persist_bonds; "

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
# Bluetooth via CC1352P7 reflash: DEAD END, see NOTES.md "Bluetooth via
# CC1352P7" (2026-08-19 update) for the full story. There used to be an
# ultima_enable_bluetooth() here installing a systemd unit doing
# `btattach -B /dev/ttyS1 -P h4 -S 115200` to attach the onboard CC1352P7
# (flashed with TI's `host_test` example firmware) as hci0. That can never
# work, and it's not a framing/flag bug:
#
#   - Confirmed from host_test's own source that it speaks TI's NPI/MT byte
#     framing (SOF/LEN/CMD0/CMD1/FCS), not raw HCI H4 — so `-P h4` was
#     already wrong.
#   - Worse, fixing the framing wouldn't be enough. TI's own BLE5-Stack
#     User's Guide (Software Architecture > BLE5-Stack Protocol Stack and
#     Application Configurations) says of the Network Processor
#     configuration host_test builds: "the network processor is not a pure
#     HCI LE controller-only implementation and the application must use TI
#     Vendor Specific HCI commands for BLE Host operations." The Vendor
#     Specific HCI Guide's HCI Interface page is more direct still: "it is
#     not possible to support an external BLE Host in the Network Processor
#     configuration." host_test runs its own GAP/GATT/SMP Host on the
#     CC1352P7 and traps the standard HCI events an external Host would
#     need. BlueZ *is* an external Host expecting a bare controller
#     underneath it — two Host stacks can't stack, no bridge daemon
#     translating NPI-framed bytes into HCI-shaped ones fixes an
#     architecture mismatch. Confirmed by reading TI's docs directly, not
#     inferred.
#   - No example under examples/rtos/LP_CC1352P7_1/ble5stack/ in SDK
#     5.30.01.01 ships a pure controller-only build (host_test,
#     simple_central, simple_peripheral, multi_role, project_zero,
#     persistent_app, simple_mesh_node* all bundle a Host). Building one
#     from source would be a separate, open-ended firmware spike, not a
#     config change here.
#
# main_uart6/ttyS1 wiring (CONFIG_GREYBUS unset + ttyport fallback,
# confirmed live via dmesg against the AM625 TRM's UART6 MMIO base) is
# still accurate and would still apply to any future controller-only
# firmware or a USB BLE dongle — that part of the investigation wasn't
# wasted, only the btattach/host_test wiring built on top of it.

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

# Bluetooth bond persistence (2026-08-20). BlueZ's default bond database
# lives at /var/lib/bluetooth, but /var/lib is one of the volatile-binds
# tmpfs mounts on this image (see "read-only-rootfs" above) — confirmed by
# reading that feature's own comment, not assumed. Left alone, every reboot
# wipes every bond, which means every drive re-triggers the pairing prompt
# BluetoothScreen.qml shows on-screen (see bluetoothmanager.cpp's
# pendingPairNeedsConfirm flow). That's not just an annoyance: a prompt the
# driver sees and taps "Accept" on every single startup stops being a real
# gate against an unintended device and becomes startup noise trained into
# reflexive acceptance. Bind-mounting /data/bluetooth (the one partition
# that survives a power cycle — see ultima-data-mount.bb) over
# /var/lib/bluetooth makes a bond made once actually stay trusted, so
# pairing only has to happen — and be deliberately accepted — once.
#
# The mkdir has to happen in its own unit ordered strictly before
# bluetooth.service, not inside bluetooth.service's own ExecStartPre:
# BindPaths= implies PrivateMounts=yes, and that mount namespace (with the
# bind already applied) is constructed once per activation and shared by
# every Exec* of that same unit, including its first ExecStartPre — so by
# the time any command in bluetooth.service itself runs, it's already too
# late for that unit to create its own bind-mount source.
ultima_bluetooth_persist_bonds () {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system
    cat > ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/ultima-bluetooth-storage.service <<'EOF'
[Unit]
Description=Create /data/bluetooth before bluetooth.service bind-mounts BlueZ bond storage onto it
After=ultima-data-mount.service
Requires=ultima-data-mount.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/mkdir -p /data/bluetooth
EOF

    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/bluetooth.service.d
    cat > ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/bluetooth.service.d/ultima-persist-bonds.conf <<'EOF'
[Unit]
After=ultima-bluetooth-storage.service
Requires=ultima-bluetooth-storage.service

[Service]
BindPaths=/data/bluetooth:/var/lib/bluetooth
EOF
}
