SUMMARY = "Nextchip NVP6324 4-channel AHD-to-MIPI-CSI2 V4L2 decoder (MY-CAM004M, CSI0)"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

inherit module

# The driver source lives in the repo's top-level camdriver/ (see
# ../../../../camdriver/PLAN.md) — bind-mounted read-only into the container by
# build.sh at /home/builder/yocto/camdriver-src and copied into WORKDIR here so
# the build never writes into the shared source tree. Same model as
# ultima-app.bb.
NVP6324_EXTERNAL_SRC = "/home/builder/yocto/camdriver-src"
S = "${WORKDIR}/camdriver-src"

# No fetched files; the source is copied in by do_unpack:append below.
SRC_URI = ""

python do_unpack:append() {
    import os, shutil
    s = d.getVar('S')
    if os.path.exists(s):
        shutil.rmtree(s)
    # Only the kernel-module bits build here; the QA app (qa/), the device-tree
    # overlay (dts/), the config fragment (kernel/) and docs are copied out.
    # .smbdelete* skip: macOS SMB rename-then-unlink tombstones (see ultima-app.bb).
    shutil.copytree(d.getVar('NVP6324_EXTERNAL_SRC'), s,
                    ignore=shutil.ignore_patterns('.smbdelete*', 'qa', 'dts',
                                                  'kernel', '*.md', '.git*'))
}

# Nothing hashes the bind-mounted source, so a source-only edit would otherwise
# reuse stale sstate — force unpack (and everything downstream) every build.
# Same trap/fix documented at length in ultima-app.bb.
do_unpack[nostamp] = "1"

# Autoload at boot. The driver is now hardware-proven: from a cold boot it probes
# cleanly and brings up a full-frame 1080p25 CSI pipeline on VC0 (CRC=0) with its
# module-param defaults (mipi_mclk=594, vc_mask=0x1, link_freq_idx=6,
# program_at_probe=1). The image bbappend no longer blacklists it.
#
# Autoload mechanism, confirmed from hardware (not assumed): KERNEL_MODULE_AUTOLOAD
# writes /usr/lib/modules-load.d/nvp6324.conf (the vendor modules-load.d dir,
# NOT /etc — verified present on the running rootfs), and systemd-modules-load
# inserts the module early at boot — journal shows `systemd-modules-load:
# Inserted module 'nvp6324'` and the probe lands at ~1.8s, before this image's
# udev coldplug (which is deliberately deferred until after the dash renders,
# see ultima-app's udev-trigger-after-dash.conf). The DT `nextchip,nvp6324`
# modalias (MODULE_DEVICE_TABLE(of, ...) in nvp6324.c) is a redundant fallback —
# udev coldplug would also match it — but modules-load wins the race here.
#
# The companion nvp6324-csi-setup oneshot (recipes-ultima) then propagates the
# CSI-2 pipeline format so a plain STREAMON on /dev/video2 works — without it
# the un-propagated pipeline fails link validation with -EPIPE (not a driver
# bug; see camdriver/nvp6324-framing-findings.md).
#
# To override a param at boot without a rebuild, drop an
# /etc/modprobe.d/nvp6324.conf `options nvp6324 ...` (e.g. mipi_mclk=1242 +
# link_freq_idx=0 for a 4x1080p build). See ../../../../camdriver/PLAN.md.
KERNEL_MODULE_AUTOLOAD += "nvp6324"
