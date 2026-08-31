#!/bin/sh
# Thin Yocto-side wrapper around mycam004m's own scripts/select-camera-backend.sh
# (installed alongside this as /usr/bin/select-camera-backend.sh). That script
# only resolves and symlinks /dev/mycam/cam1..4 — it deliberately doesn't
# decide real-vs-fake itself. This one reads that choice from /data (the only
# persistent, writable partition — see beagley-ai/NOTES.md), defaulting to
# "real": MY-CAM004M hardware is cabled to CSI0 and was hardware-verified
# streaming on this board 2026-08-31 (see ~/code/mycam004m README's
# "BeagleY-AI port" section).
#
# To switch persistently: `echo fake > /data/camera-backend` then reboot (or
# re-run this script). To switch live without touching the persisted choice:
# run `select-camera-backend.sh fake` directly instead of this wrapper.
#
# Real-backend wrinkle (bench, 2026-08-31): the ticsi2rx capture nodes appear
# asynchronously — the v4l2-async media graph only registers them once
# mycam004m and the whole in-tree CSI stack have all bound — usually *after*
# this early oneshot runs. This unit is ordered Before=ultima-app.service and
# must stay cheap (boot-to-first-frame is a hard project metric), so it does
# NOT wait: a real resolve that finds nothing is fine here, because
# mycam004m-configure-pipeline.service runs later, waits for the nodes,
# re-runs the resolve, and sets the subdev formats streaming needs anyway.
# Fake nodes, by contrast, exist as soon as systemd-modules-load finishes, so
# a failed fake resolve is a genuine error and stays fatal.
set -e

backend=real
[ -r /data/camera-backend ] && backend="$(cat /data/camera-backend)"

if [ "$backend" = "real" ]; then
	/usr/bin/select-camera-backend.sh real || echo \
		"real capture nodes not up yet; mycam004m-configure-pipeline.service finishes the job" >&2
	exit 0
fi

exec /usr/bin/select-camera-backend.sh "$backend"
