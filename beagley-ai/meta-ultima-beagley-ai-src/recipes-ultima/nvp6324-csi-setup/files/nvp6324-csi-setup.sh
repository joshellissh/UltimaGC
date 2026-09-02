#!/bin/sh
# Propagate the NVP6324 CSI-2 pipeline format so a plain STREAMON on the
# capture node succeeds.
#
# WHY THIS EXISTS. The NVP6324 subdev sources UYVY 1920x1080 on its MIPI pad,
# but the downstream Cadence CSI2RX bridge and TI CSI2RX SHIM pads come up at
# their 640x480 default. V4L2 link validation compares adjacent pad formats at
# STREAMON, so an un-propagated pipeline fails with -EPIPE ("Broken pipe")
# even though the chip is locked and streaming perfectly. media-ctl must push
# the format down the chain once; it then persists in each subdev's active
# state across STREAMOFF/STREAMON, so this is a boot-time one-shot. Routing
# (nvp6324 -> bridge -> SHIM context 0 = /dev/video2 = VC0) is already set up
# by the driver + DT (ENABLED,IMMUTABLE), so only the format needs setting.
#
# This lives in the board layer, not ultima-app: the media entity names below
# are tied to this SoC (J722S) and its device tree, whereas ultima-app is
# board-agnostic (see CLAUDE.md).
#
# Current config is VC0-only, 1080p25 (driver defaults vc_mask=0x1,
# mipi_mclk=594, link_freq_idx=6). A 4-camera build (vc_mask=0xF,
# mipi_mclk=1242) additionally needs the same format pushed onto SHIM contexts
# 1-3 (/dev/video3..5) via the bridge's per-stream source pads; see the stub
# at the bottom.
set -e

MEDIA=/dev/media0
FMT="fmt:UYVY8_1X16/1920x1080"
SRC="nvp6324 4-0031"                          # i2c bus 4, addr 0x31 (fixed)
BRIDGE="cdns_csi2rx.30101000.csi-bridge"      # fixed SoC address
SHIM="30102000.ticsi2rx"                       # fixed SoC address

log() { echo "nvp6324-csi-setup: $*"; }

graph_ready() {
	[ -e "$MEDIA" ] && media-ctl -d "$MEDIA" -p 2>/dev/null | grep -q "entity.*$SRC"
}

# The Cadence CSI2RX bridge finishes its async probe ~6.5s after boot (the
# nvp6324 i2c subdev itself probes at ~1.8s); the media graph is not complete
# until then. Wait, bounded (~20s), for the source entity to appear rather
# than racing it. A missing camera is not fatal to the rest of the boot.
i=0
while [ "$i" -lt 40 ]; do
	graph_ready && break
	i=$((i + 1))
	sleep 0.5
done
if ! graph_ready; then
	# No camera by the deadline: log and succeed. A wired-but-absent camera
	# must not fail the boot; a genuine format rejection below still does (via
	# set -e), so the two failure modes stay distinguishable in the journal.
	log "'$SRC' not present in $MEDIA after 20s; leaving pipeline unset"
	exit 0
fi

# Push 1080p UYVY down the VC0 path. Setting the bridge sink propagates to its
# source pad internally; the SHIM sink is set explicitly. media-ctl returns
# non-zero on a rejected format, so `set -e` fails the unit if any step is
# refused (e.g. a resolution the pipeline can't carry).
media-ctl -d "$MEDIA" -V "\"$SRC\":4/0 [$FMT]"
media-ctl -d "$MEDIA" -V "\"$BRIDGE\":0/0 [$FMT]"
media-ctl -d "$MEDIA" -V "\"$SHIM\":0/0 [$FMT]"

log "VC0 pipeline set to UYVY 1920x1080 (/dev/video2 ready)"

# --- 4-VC build (vc_mask=0xF) extension, left disabled for the VC0 default ---
# The bridge demuxes VC0..3 onto source-pad streams 0..3, each bound to a SHIM
# context (video2..5). Uncomment and adjust once a 4-camera build is proven:
#   media-ctl -d "$MEDIA" -V "\"$BRIDGE\":0/1 [$FMT]"
#   media-ctl -d "$MEDIA" -V "\"$BRIDGE\":0/2 [$FMT]"
#   media-ctl -d "$MEDIA" -V "\"$BRIDGE\":0/3 [$FMT]"
