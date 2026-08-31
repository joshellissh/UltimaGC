#!/bin/sh
# Boot-time bring-up for the *real* mycam004m backend. Three jobs, all
# hardware-verified on this board 2026-08-31 (see ~/code/mycam004m
# docs/testing.md "Set the format on the CSI-2 bridge and shim" and
# beagley-ai/NOTES.md):
#
#  1. Wait (bounded) for the four ticsi2rx capture nodes. They register
#     asynchronously after systemd-modules-load returns — the v4l2-async
#     graph completes only once mycam004m + cdns-csi2rx + j721e-csi2rx have
#     all bound — which is exactly the race that made the first real-backend
#     bench boot come up with no /dev/mycam links.
#  2. Re-run the symlink resolve. Node numbers shift between boots (cam1 has
#     been /dev/video6 and /dev/video4 on consecutive bench boots), so the
#     resolve must happen after the nodes exist, from their sysfs names.
#  3. Set the active subdev format on the two SoC-side CSI entities. They
#     boot with a default UYVY8_1X16/640x480 that never auto-propagates from
#     mycam004m's own (correct) pads, and until they match what the camera
#     actually sends, VIDIOC_STREAMON on any capture node fails with EPIPE.
#     Only pad 0 / stream 0 (AHD input 0) is configured — one physical
#     camera is attached; shipping untested multi-route config for the empty
#     inputs is the same thing the BeaglePlay-era script deliberately
#     avoided. Revisit when more cameras are cabled (needs -R route calls
#     plus a per-stream -V each, see docs/testing.md).
#
# Fake backend: nothing to do — mycam004m-select-backend.service already
# resolved the links, and the fake nodes need no media-controller setup.
set -eu

backend=real
[ -r /data/camera-backend ] && backend="$(cat /data/camera-backend)"
[ "$backend" = "real" ] || exit 0

# 1. Wait for the real capture nodes ("30102000.ticsi2rx context N"; the
# BeagleY ticsi2rx registers 6 contexts, only 0..3 are the four AHD inputs).
tries=0
while :; do
	n=0
	for name_file in /sys/class/video4linux/video*/name; do
		[ -e "$name_file" ] || continue
		case "$(cat "$name_file")" in
		mycam004m-fake*) ;;
		*" context "[0-3]) n=$((n + 1)) ;;
		esac
	done
	[ "$n" -ge 4 ] && break
	tries=$((tries + 1))
	if [ "$tries" -gt 15 ]; then
		echo "timed out waiting for ticsi2rx capture nodes ($n of 4 up)" >&2
		echo "is the CSI media graph complete? (media-ctl -p; dmesg | grep -i -e mycam -e csi)" >&2
		exit 1
	fi
	sleep 1
done

# 2. Resolve /dev/mycam/cam1..4 against the nodes that just appeared.
/usr/bin/select-camera-backend.sh real

# 3. Subdev formats. Don't hardcode /dev/media0 — find the media device that
# actually owns the ticsi2rx entity.
mdev=
for m in /dev/media*; do
	[ -e "$m" ] || continue
	if media-ctl -d "$m" -p 2>/dev/null | grep -q "30102000.ticsi2rx"; then
		mdev="$m"
		break
	fi
done
if [ -z "$mdev" ]; then
	echo "no /dev/media* device exposes 30102000.ticsi2rx" >&2
	exit 1
fi

media-ctl -d "$mdev" -V '"cdns_csi2rx.30101000.csi-bridge":0/0 [fmt:UYVY8_1X16/1920x1080 field:none]'
media-ctl -d "$mdev" -V '"30102000.ticsi2rx":0/0 [fmt:UYVY8_1X16/1920x1080 field:none]'

echo "real camera pipeline configured on $mdev (input 0, UYVY8_1X16/1920x1080)"
