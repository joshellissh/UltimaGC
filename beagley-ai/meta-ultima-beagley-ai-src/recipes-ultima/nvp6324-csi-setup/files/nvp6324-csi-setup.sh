#!/bin/sh
# Route + format the NVP6324 CSI-2 pipeline so a plain STREAMON on each of the
# four capture nodes (/dev/video2..5) succeeds at boot.
#
# WHY THIS EXISTS. The NVP6324 subdev sources UYVY 1920x1080 on its MIPI pad,
# but the downstream Cadence CSI2RX bridge and TI CSI2RX SHIM pads come up at
# their 640x480 default. V4L2 link validation compares adjacent pad formats at
# STREAMON, so an un-propagated pipeline fails with -EPIPE ("Broken pipe")
# even though the chip is locked and streaming perfectly. media-ctl must push
# the format down the chain once; it then persists in each subdev's active
# state across STREAMOFF/STREAMON, so this is a boot-time one-shot.
#
# ROUTING. The driver + DT wire ONLY VC0 through the bridge and SHIM
# (ENABLED,IMMUTABLE). This board runs 4 AHD cameras on CH0-CH3 (arbiter
# vc_mask=0xF, mipi_mclk=1049 -- see recipes-kernel/nvp6324/files/nvp6324.conf),
# so VC1/VC2/VC3 also need explicit routes: the Cadence bridge demuxes the 4
# VCs (its sink pad0 streams 0/1/2/3) out its single source pad1 as streams
# 0/1/2/3, and the SHIM splits those onto contexts 0/1/2/3 = /dev/video2/3/4/5.
#
# This lives in the board layer, not ultima-app: the media entity names below
# are tied to this SoC (J722S) and its device tree, whereas ultima-app is
# board-agnostic (see CLAUDE.md).
#
# Kept in lock-step with the driver's vc_mask (0xF = 4 cameras). If vc_mask
# ever changes, update the routes here to match the populated channels, else
# an enabled-but-camera-less VC free-runs and every frame splits (fps doubles,
# image tears) -- see camdriver/nvp6324-framing-findings.md.
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
	# must not fail the boot; a genuine format/route rejection below still does
	# (via set -e), so the two failure modes stay distinguishable in the journal.
	log "'$SRC' not present in $MEDIA after 20s; leaving pipeline unset"
	exit 0
fi

# Demux VC0/1/2/3. NB: media-ctl -R rejects the name-attached form ("name[...]")
# with EINVAL; use the quoted entity name followed by a space and the route
# list. active flag = [1]. SHIM source pads 1/2/3/4 = contexts 0/1/2/3.
media-ctl -d "$MEDIA" -R "\"$BRIDGE\" [0/0->1/0[1],0/1->1/1[1],0/2->1/2[1],0/3->1/3[1]]"
media-ctl -d "$MEDIA" -R "\"$SHIM\" [0/0->1/0[1],0/1->2/0[1],0/2->3/0[1],0/3->4/0[1]]"

# Push 1080p UYVY down all four stream paths. The -R routing above resets each
# pad's stream-0 format to the 640x480 default, so VC0 (stream 0) MUST be set
# here too or STREAMON on /dev/video2 EPIPEs. Setting a subdev's sink stream
# propagates to its source pad internally. media-ctl returns non-zero on a
# rejected format, so `set -e` fails the unit if any step is refused.
for s in 0 1 2 3; do
	media-ctl -d "$MEDIA" -V "\"$SRC\":4/$s [$FMT]"
	media-ctl -d "$MEDIA" -V "\"$BRIDGE\":0/$s [$FMT]"
	media-ctl -d "$MEDIA" -V "\"$SHIM\":0/$s [$FMT]"
done

log "VC0/1/2/3 routed + set to UYVY 1920x1080 (/dev/video2..5 ready)"
