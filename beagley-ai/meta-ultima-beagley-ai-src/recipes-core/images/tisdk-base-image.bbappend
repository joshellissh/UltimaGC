# BeagleY-AI (AM67A/J722S) bring-up (2026-08-28), milestone 1 (Yocto
# build/app bring-up, no board in hand yet — see docs/BEAGLEY-AI-EVAL.md and
# beagleplay-falcon/NOTES.md "BeagleY-AI bring-up"). Split into this separate
# layer from meta-ultima-beagleplay (2026-08-28) — same "unrelated concerns"
# reasoning meta-falcon-beagleplay already used to stay apart from the app
# layer, applied here so this directory isn't misleadingly named after the
# other board. meta-ultima-beagleplay's own tisdk-base-image.bbappend still
# carries the machine-agnostic image-hardening bits (read-only-rootfs,
# ROOTFS_POSTPROCESS_COMMAND and friends) that apply to both machines
# unconditionally — only the beagleplay-ti/beagley-ai-scoped IMAGE_INSTALL
# and WKS_FILE lines split out.
#
# Deliberately narrower than the beagleplay-ti set:
# - ultima-hwclock-load dropped: it hardcodes /dev/rtc0 = the onboard
#   BQ32002, BeaglePlay-specific hardware. This board's KERNEL_DEVICETREE
#   (see meta-beagle's beagley-ai.conf) has an *optional* RV3028 RTC overlay
#   (k3-am67a-beagley-ai-i2c1-rtc-rv3028.dtbo) — not applied here, revisit
#   if/when that hardware is actually present.
# - WiFi (wpa-supplicant/wl18xx-firmware) and mycam004m: skipped,
#   BeaglePlay-specific hardware (WL1807) / a later milestone (camera port),
#   not this one.
IMAGE_INSTALL:append:beagley-ai = " ultima-app ultima-splash can-utils mmc-utils ultima-data-mount volatile-binds"

# Same GPU smoke-test packages as beagleplay-ti's own copy of this line —
# this board's BXS-4-64 Rogue stack is genuinely unproven (no board in hand
# yet as of 2026-08-28), so keeping kmscube/mesa-demos here for the same
# verify-before-blaming-the-app reason, doubly so on unproven hardware.
# Remove alongside the beagleplay-ti copy once both are confirmed working.
IMAGE_INSTALL:append:beagley-ai = " ti-img-rogue-driver ti-img-rogue-umlibs kmscube mesa-demos"

# Boot-time work (2026-08-30): ultima-readahead replays the recorded startup
# working set (see recipes-ultima/ultima-readahead); systemd-analyze is here
# so `systemd-analyze blame/critical-chain/plot` can be run on the board
# instead of reconstructing the unit timeline from the journal by hand.
IMAGE_INSTALL:append:beagley-ai = " ultima-readahead systemd-analyze"

# See wic/ultima-beagley-ai.wks.in (this layer) for the full reasoning —
# straight copy of ultima-beagleplay.wks.in, nothing AM625-specific in it.
# WKS_SEARCH_PATH covers this layer's own wic/ dir the same way it already
# covers meta-ultima-beagleplay's (confirmed via `bitbake -e
# tisdk-base-image` after the split, same check meta-ultima-beagleplay's
# own comment on this line already used).
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
# the finished rootfs the same way meta-ultima-beagleplay's
# ultima_mask_timesyncd does it (a /dev/null symlink wins over any enable
# pass). None of these run before the app, but every one of them was
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
