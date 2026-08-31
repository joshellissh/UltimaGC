FILESEXTRAPATHS:prepend := "${THISDIR}/linux-bb.org:"
SRC_URI:append = " file://0001-arm64-dts-k3-am67a-beagley-ai-enable-usb1-host.patch"

# BeagleY-AI's own upstream board dts (bb.org kernel, not this project's own
# source) leaves usbss1/usb1 (the SoC's only USB3 host/OTG controller, feeding
# the board's 4 physical USB-A ports through an onboard hub) and its
# underlying serdes_wiz0 PHY block both `status = "disabled"` — confirmed on
# real hardware: zero xhci/cdns/wiz lines anywhere in dmesg, empty
# /sys/bus/usb/devices (not even a root hub), so nothing USB-attached (the
# Waveshare touchscreen included) can ever be detected. Not a Qt/QPA/HID
# problem — one level below that, the USB3 controller itself never probes.
#
# TI's own same-SoC (J722S) EVM board dts already carries the exact fix
# (`k3-j722s-evm.dts`): enable serdes_wiz0, then usbss1/usb1 with the
# dedicated USB1_DRVVBUS pin (J722S_IOPAD 0x0258 — a fixed SoC pin, not
# board-routed, so no BeagleY-AI-specific schematic lookup was needed) and
# the already-present serdes0_usb_link phy (this board's dts already wires
# SERDES0 lane0 to USB3 and sets `&serdes0 { status = "okay" }` — only the
# wiz wrapper and the controller itself were left off). Ported verbatim.
#
# Verified live on hardware before baking in (same discipline as the
# grub.cfg display fix): compiled as a standalone `fdtoverlay` against the
# deployed dtb, pushed over SSH, rebooted — xHCI host controller registers,
# internal hub enumerates, Waveshare touchscreen shows up as hid-multitouch
# with a working /dev/input/eventN. See NOTES.md for the full diagnosis and
# verification evidence.
#
# `&serdes_wiz1` (PCIe's own copy of the same wiz block) has the identical
# gap and was deliberately NOT touched here — same root cause, but PCIe is
# unrelated to this fix and out of scope; see NOTES.md.

# Boot-time config trims (2026-08-30) — see ultima-boot.cfg for the per-option
# reasoning and the initcall_debug measurements behind it. setup-defconfig.inc
# merges KERNEL_CONFIG_FRAGMENTS onto bb.org_defconfig with merge_config.sh
# and re-runs oldconfig, same mechanism meta-beagle's own no-fortify.cfg uses.
SRC_URI:append = " file://ultima-boot.cfg"
KERNEL_CONFIG_FRAGMENTS += "${WORKDIR}/ultima-boot.cfg"

# Camera port (2026-08-30): wires the MYIR MY-CAM004M decoder (mycam004m.ko,
# recipes-kernel/mycam004m/) onto the CSI0 connector — main_i2c2 plus
# cdns_csi2rx0/dphy0/ti_csi2rx0. This board boots via Falcon (see the notes
# above the USB1 patch, and beagley-ai/NOTES.md): there is no U-Boot proper,
# no GRUB, no runtime devicetree-overlay mechanism at all — the R5 SPL loads
# one FIT with the DTB already baked in. So, same as the USB1 fix, this is a
# source patch against the base .dts, not a .dtbo applied at boot.
#
# Two things this patch does NOT resolve, both flagged inline in the patch
# itself and in mycam004m's own README ("BeagleY-AI port" section):
# 1. Which of J722S's four CSI2RX/D-PHY instances CSI0 is physically wired
#    to. No schematic confirming it was found. Targets instance 0 as the
#    natural first guess (`gh search code --repo beagleboard/linux` and
#    TI's own upstream reference overlays came up empty on this specific
#    fact) — if probe succeeds (dmesg: "found N4 decoder: DEV_ID 0xb0") but
#    video never locks with a real MY-CAM004M cabled in, instance 1
#    (cdns_csi2rx1/dphy1/ti_csi2rx1) is the thing to try next.
# 2. reset-gpios/pwren-gpios are omitted — CSI0 appears to expose no spare
#    GPIOs (unlike BeaglePlay's J17, which routes two directly on the
#    connector). Both are optional in the driver, and BeaglePlay's own
#    hardware-verified single-camera capture worked with both unset too, so
#    there's real precedent this doesn't block a signal lock — but it's
#    unconfirmed on this board specifically.
#
# Verified before landing: the driver builds clean (W=1) against this exact
# kernel tree, and — the strongest check available without hardware — this
# patch applied and the FULL board .dts (not just a standalone overlay)
# compiles clean through this tree's own cpp + scripts/dtc/dtc, decompiling
# back to confirm camera@30 lands under i2c@20020000 (main_i2c2) with its
# endpoint correctly cross-linked to cdns_csi2rx0's port@0. Not verified,
# and can't be without real MY-CAM004M hardware cabled to CSI0: whether a
# signal actually locks. See mycam004m's own README for the full trail.
SRC_URI:append = " file://0002-arm64-dts-k3-am67a-beagley-ai-add-mycam004m-csi0.patch"
