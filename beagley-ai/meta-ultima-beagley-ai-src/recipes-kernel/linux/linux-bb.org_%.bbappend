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

# NVP6324 / MY-CAM004M CSI-2 capture stack (CSI0) — pin the in-tree Cadence
# CSI2RX + TI SHIM + Cadence D-PHY-RX + V4L2 media core the out-of-tree NVP6324
# module (recipes-kernel/nvp6324) binds onto. Already =m/=y in the current
# defconfig; asserted so it can't silently regress. See nvp6324.cfg and
# ../../../../camdriver/PLAN.md.
SRC_URI:append = " file://nvp6324.cfg"
KERNEL_CONFIG_FRAGMENTS += "${WORKDIR}/nvp6324.cfg"
