# BeagleY-AI (AM67A/J722S) image customizations. This bbappend carries the
# machine-scoped IMAGE_INSTALL / WKS_FILE / boot-files lines below, plus the
# machine-agnostic image hardening (read-only-rootfs + the unit-mask
# functions) folded in at the bottom of this file. See the BeagleY-AI notes.
#
# The install set deliberately excludes some things:
# - ultima-hwclock-load dropped: it hardcodes /dev/rtc0 = an onboard BQ32002,
#   which this board doesn't have. This board's KERNEL_DEVICETREE (see
#   meta-beagle's beagley-ai.conf) has an *optional* RV3028 RTC overlay
#   (k3-am67a-beagley-ai-i2c1-rtc-rv3028.dtbo) — not applied here, revisit
#   if/when that hardware is actually present.
# - WiFi (wpa-supplicant/wl18xx-firmware): skipped — hardware this board
#   doesn't have (WL1807).
IMAGE_INSTALL:append:beagley-ai = " ultima-app ultima-splash can-utils mmc-utils ultima-data-mount volatile-binds"

# Camera port (2026-08-30, real backend default since 2026-08-31):
# mycam004m.ko (real, CSI0 — see recipes-kernel/linux/linux-bb.org_%.bbappend
# for the devicetree side) plus mycam004m-fake.ko (static test images). Real
# MY-CAM004M hardware is cabled to CSI0 and bench-verified streaming, so
# recipes-kernel/mycam004m/mycam004m.bb now boots the real backend by default
# (echo fake > /data/camera-backend to switch back) and ships
# mycam004m-configure-pipeline.service, the media-ctl bring-up the real
# backend needs before any STREAMON works.
IMAGE_INSTALL:append:beagley-ai = " mycam004m"

# Dashcam recording, storage side (2026-08-30): auto-mounts a USB drive
# labeled ULTIMA_DVR to /mnt/dvr whenever one is plugged in — see
# recipes-ultima/ultima-dvr-mount. CONFIG_EXFAT_FS=m is already in this
# kernel's .config and kernel-module-exfat is already pulled onto this image
# (confirmed via buildhistory, neither added by this layer) — exFAT chosen
# for cross-platform (Mac/Windows/Linux) browsability of the recorded files,
# per the dashcam-recording proposal. The recording pipeline itself
# (Wave5 encode, segment rotation) is later work — this is storage plumbing
# only, and doesn't depend on it.
IMAGE_INSTALL:append:beagley-ai = " ultima-dvr-mount"

# GPU smoke-test packages for this board's BXS-4-64 Rogue stack — keep
# kmscube/mesa-demos here for the verify-the-GPU-before-blaming-the-app
# reason. Remove once the GPU stack is confirmed working; they don't belong
# in a shipped image.
IMAGE_INSTALL:append:beagley-ai = " ti-img-rogue-driver ti-img-rogue-umlibs kmscube mesa-demos"

# Boot-time work (2026-08-30): ultima-readahead replays the recorded startup
# working set (see recipes-ultima/ultima-readahead); systemd-analyze is here
# so `systemd-analyze blame/critical-chain/plot` can be run on the board
# instead of reconstructing the unit timeline from the journal by hand.
IMAGE_INSTALL:append:beagley-ai = " ultima-readahead systemd-analyze"

# See wic/ultima-beagley-ai.wks.in (this layer) for the full reasoning.
# WKS_SEARCH_PATH covers this layer's own wic/ dir (confirmed via `bitbake -e
# tisdk-base-image`).
WKS_FILE:beagley-ai = "ultima-beagley-ai.wks.in"

# Falcon boot (2026-08-30, hardware-verified): the boot partition carries only
# the falcon-configured R5 SPL and the FIT it loads (ATF + OP-TEE + DM +
# kernel Image + DTB). Replaces k3.inc's default of tispl.bin/u-boot.img plus
# bootimg-efi's GRUB/Image/dtb copies — none of those run anymore. wic's
# bootimg-partition source copies IMAGE_BOOT_FILES out of DEPLOY_DIR_IMAGE,
# so tifalcon.bin must have been deployed first. See
# recipes-bsp/ultima-falcon-fit and wic/ultima-beagley-ai.wks.in.
IMAGE_BOOT_FILES:beagley-ai = "tiboot3.bin tifalcon.bin"
do_image_wic[depends] += "${@bb.utils.contains('MACHINE', 'beagley-ai', 'ultima-falcon-fit:do_deploy', '', d)}"

# Boot-time trim (2026-08-30): services the dash has no use for, masked in
# the finished rootfs the same /dev/null-symlink way as ultima_mask_timesyncd
# (defined at the bottom of this file) — a /dev/null symlink wins over any
# enable pass. None of these run before the app, but every one of them was
# starting in the same window as the app's first frame, and the unit files
# are parsed by systemd at every boot regardless. Networking itself
# (systemd-networkd, dropbear) stays: wired SSH is the bench path.
# - rpcbind / nfs-statd / remote-fs: no NFS on this image (NFS_FS is off in
#   ultima-boot.cfg too).
# - avahi: mDNS advertising; the board is reached by IP.
# - iptables / ip6tables: meta-arago's empty-ruleset loaders.
# - docker.socket: stray socket unit from the base image, no docker here.
# - gplv3-notice: prints a licence banner to the console at boot.
# - systemd-networkd-wait-online: blocks network-online.target for up to
#   2 min when no link is up — in a car there is none.
# - systemd-resolved: DNS stub for a box that only ever talks to an IP.
ROOTFS_POSTPROCESS_COMMAND:append:beagley-ai = " ultima_beagley_mask_units; ultima_beagley_drop_generators; "

ultima_beagley_mask_units () {
    for u in rpcbind.service rpcbind.socket nfs-statd.service remote-fs.target \
             avahi-daemon.service avahi-daemon.socket iptables.service ip6tables.service \
             docker.socket gplv3-notice.service systemd-networkd-wait-online.service \
             systemd-resolved.service; do
        rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/*.wants/$u
        ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/$u
    done
}

# systemd runs every generator in system-generators/ before it can queue the
# first job — nine fork+execs off a cold SD card, in series, on the critical
# path. Six of them have nothing to do here: gpt-auto (partition
# auto-discovery; the root and /data are stated explicitly), hibernate-resume,
# system-update, rc-local (no /etc/rc.local), debug (systemd.debug_shell=),
# run (systemd.run=). Measured on hardware 2026-08-30: "Hostname set" →
# "Queued start job" 0.53 → 0.455 s. Kept: fstab (mounts), getty (creates
# serial-getty@ttyS2 from console=), sysv (/etc/init.d/telnetd).
ultima_beagley_drop_generators () {
    for g in systemd-gpt-auto-generator systemd-hibernate-resume-generator \
             systemd-system-update-generator systemd-rc-local-generator \
             systemd-debug-generator systemd-run-generator; do
        rm -f ${IMAGE_ROOTFS}${nonarch_libdir}/systemd/system-generators/$g
    done
}

# --- Machine-agnostic image hardening (folded in when BeagleY-AI became the
# sole board, 2026-08-30 — previously an unconditional block in a shared
# layer that has since been retired). read-only rootfs plus a handful of
# surgical unit masks; none of it is board-specific.
# The WL1807 WiFi enablement that used to ride the same
# ROOTFS_POSTPROCESS_COMMAND line was dropped in the move: this board has no
# wpa-supplicant / wl18xx-firmware installed (see IMAGE_INSTALL above), so it
# only ever wrote dead config and a dangling wpa_supplicant@wlan0 symlink.

# read-only-rootfs: root (p2) becomes mount-time read-only. oe-core's
# rootfs-postcommands.bbclass does most of the work once the feature is on: it
# appends "ro" to the kernel cmdline, rewrites /etc/fstab's /dev/root line to
# ro, empties /etc/machine-id at build time so systemd's transient-ID support
# takes over, and redirects dropbear to generate its host key under
# /var/lib/dropbear. The one thing that mechanism assumes and this distro
# doesn't provide on its own is somewhere writable for that state to land --
# volatile-binds (stock oe-core, in IMAGE_INSTALL above) bind-mounts tmpfs
# over /var/lib, /var/cache, /var/spool and /srv for exactly that. Accepted
# consequence: machine-id and the dropbear host key become transient,
# regenerated every boot, since nothing here persists /var/lib across reboots.
IMAGE_FEATURES += "read-only-rootfs"

# Surgical tweaks to the finished rootfs. Each runs after all package
# postinsts (including systemd's own "enable" pass), so a symlink-to-/dev/null
# mask wins regardless of how a unit would otherwise be pulled in.
ROOTFS_POSTPROCESS_COMMAND += "ultima_mask_timesyncd; ultima_journald_volatile; ultima_coredump_disable; ultima_mask_resize_rootfs; ultima_mask_getty_tty1; "

# systemd-timesyncd silently overwrites whatever SetTimeScreen just wrote via
# SystemClock::setTime() any time the board has network. It ships inside the
# base "systemd" package in this build (not a split systemd-timesyncd
# sub-package), so SYSTEMD_AUTO_ENABLE has no package to attach to -- masking
# the unit directly is the surgical way to disable exactly it.
ultima_mask_timesyncd () {
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/multi-user.target.wants/systemd-timesyncd.service
    ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/systemd-timesyncd.service
}

# meta-ti-foundational's resize_rootfs.service grows partition 2 (hardcoded
# `sfdisk -N 2 ... echo ",+"`) into whatever free space follows it on first
# boot. The image adds /data as a real partition 3 after root (see
# wic/ultima-beagley-ai.wks.in), so that free-space grab would race /data for
# the same space. Masking removes the collision, and a bounded root size is
# the actual goal of the read-only-rootfs setup this supports.
ultima_mask_resize_rootfs () {
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/multi-user.target.wants/resize_rootfs.service
    ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/resize_rootfs.service
}

# Keep the journal on tmpfs (/run), never flushing to disk -- write
# amplification a read-only root is meant to avoid, and the documented cause
# of a real SD-card I/O-error incident during an early crash-loop.
ultima_journald_volatile () {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/journald.conf.d
    printf '[Journal]\nStorage=volatile\n' > ${IMAGE_ROOTFS}${sysconfdir}/systemd/journald.conf.d/ultima-volatile.conf
}

# Companion to ultima_journald_volatile: drop core dumps instead of writing
# them to /var/lib/systemd/coredump. Harmless no-op if systemd-coredump isn't
# installed; kept so it isn't a gap if that changes later.
ultima_coredump_disable () {
    install -d ${IMAGE_ROOTFS}${sysconfdir}/systemd/coredump.conf.d
    printf '[Coredump]\nStorage=none\n' > ${IMAGE_ROOTFS}${sysconfdir}/systemd/coredump.conf.d/ultima-disable.conf
}

# getty@tty1 writes a login prompt onto tty1 -- the same VT fbcon and the
# fbdev splash draw into -- a couple seconds into boot, overwriting the splash
# pixels. The only interactive access here is serial (serial-getty@, a
# separate template unit this doesn't touch) or SSH, so mask the tty1 getty.
ultima_mask_getty_tty1 () {
    ln -sf /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/getty@tty1.service
}
