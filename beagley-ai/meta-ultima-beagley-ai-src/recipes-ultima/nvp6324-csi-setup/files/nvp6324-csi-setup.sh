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

# Routing + format are applied by apply_pipeline(), retried below. The
# nvp6324 entity showing up in the media graph does not guarantee everything
# media-ctl needs is ready yet (seen on hardware: a boot where the first -R
# failed with "Unable to setup routes: No such file or directory", leaving the
# 640x480 defaults and every camera STREAMON -EPIPE until the script was re-run
# by hand once boot had settled -- most likely a bridge/SHIM entity or its
# /dev/v4l-subdevN node not registered yet). The whole sequence is idempotent
# (-R replaces the route table, -V sets absolute formats), so just retry it.
#
# Demux VC0/1/2/3. NB: media-ctl -R rejects the name-attached form ("name[...]")
# with EINVAL; use the quoted entity name followed by a space and the route
# list. active flag = [1]. SHIM source pads 1/2/3/4 = contexts 0/1/2/3.
#
# Then push 1080p UYVY down all four stream paths. The -R routing resets each
# pad's stream-0 format to the 640x480 default, so VC0 (stream 0) MUST be set
# here too or STREAMON on /dev/video2 EPIPEs. Setting a subdev's sink stream
# propagates to its source pad internally. media-ctl returns non-zero on a
# rejected format, so each step aborts the attempt on failure. NB: explicit
# `|| exit 1`, not `set -e` -- errexit is ignored inside a function called from
# an `until`/`if` condition (it would silently run on past a failed -R).
apply_pipeline() (
	media-ctl -d "$MEDIA" -R "\"$BRIDGE\" [0/0->1/0[1],0/1->1/1[1],0/2->1/2[1],0/3->1/3[1]]" || exit 1
	media-ctl -d "$MEDIA" -R "\"$SHIM\" [0/0->1/0[1],0/1->2/0[1],0/2->3/0[1],0/3->4/0[1]]" || exit 1
	for s in 0 1 2 3; do
		media-ctl -d "$MEDIA" -V "\"$SRC\":4/$s [$FMT]" || exit 1
		media-ctl -d "$MEDIA" -V "\"$BRIDGE\":0/$s [$FMT]" || exit 1
		media-ctl -d "$MEDIA" -V "\"$SHIM\":0/$s [$FMT]" || exit 1
	done
)

# One deadline loop does both the waiting and the retrying. The Cadence CSI2RX
# bridge finishes its async probe ~6.5-7s after boot (the nvp6324 i2c subdev
# itself probes at ~1.8s) and the media graph is not complete until then. Each
# pass: if the graph shows the camera, try to apply the pipeline; success is
# only ever "graph_ready AND apply_pipeline both succeeded in the same pass".
#
# Do NOT split this into "wait for graph_ready, then check it again and decide
# the camera is absent". That was the previous shape, and on hardware it gave up
# ~1s after the bridge probed with "not present after 20s" (the unit had run for
# 2.3s, not 20): the wait loop broke on a true graph_ready, and an immediate
# second graph_ready came back false while the graph was still being assembled
# (media-ctl -p reads the graph mid-registration). A single check is not a
# reliable "absent" verdict, so absence is only concluded after the deadline.
#
# Deadline is read off /proc/uptime rather than counted in iterations, so it is a
# real wall-clock bound however long each media-ctl call takes under boot load.
now() { cut -d. -f1 /proc/uptime; }
deadline=$(( $(now) + 30 ))
attempt=0
seen=
while :; do
	attempt=$((attempt + 1))
	if graph_ready; then
		seen=1
		if apply_pipeline; then
			log "VC0/1/2/3 routed + set to UYVY 1920x1080 (/dev/video2..5 ready, attempt $attempt)"
			exit 0
		fi
		log "attempt $attempt: graph present but routing/format failed; retrying"
	fi
	[ "$(now)" -ge "$deadline" ] && break
	sleep 0.5
done

if [ -n "$seen" ]; then
	# The camera was in the graph but the pipeline could never be applied: a
	# real failure, so the unit shows failed.
	log "routing/format still failing after $attempt attempts (30s); giving up"
	exit 1
fi
# Never seen in the graph within the deadline. A wired-but-absent camera must
# not fail the boot, so succeed -- logged, and distinct from the failure above.
log "'$SRC' not present in $MEDIA after 30s ($attempt checks); leaving pipeline unset"
exit 0
