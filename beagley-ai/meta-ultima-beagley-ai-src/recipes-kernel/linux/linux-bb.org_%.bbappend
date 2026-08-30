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
