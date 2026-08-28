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

# See wic/ultima-beagley-ai.wks.in (this layer) for the full reasoning —
# straight copy of ultima-beagleplay.wks.in, nothing AM625-specific in it.
# WKS_SEARCH_PATH covers this layer's own wic/ dir the same way it already
# covers meta-ultima-beagleplay's (confirmed via `bitbake -e
# tisdk-base-image` after the split, same check meta-ultima-beagleplay's
# own comment on this line already used).
WKS_FILE:beagley-ai = "ultima-beagley-ai.wks.in"
