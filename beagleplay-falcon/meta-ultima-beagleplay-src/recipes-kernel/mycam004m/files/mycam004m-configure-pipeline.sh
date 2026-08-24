#!/bin/sh
# Sets the active v4l2-subdev format on the two entities between mycam004m
# and the 4 capture nodes that don't auto-propagate it from the camera's own
# (correctly self-initialized) pads -- confirmed on real hardware
# (2026-08-23, see NOTES.md): without this, VIDIOC_STREAMON on any
# /dev/mycam/camN fails with EPIPE, because "cdns_csi2rx...csi-bridge" and
# "...ticsi2rx" both boot with an uninitialized default active format
# (UYVY8_1X16/640x480, wrong size) that doesn't match what mycam004m is actually
# sending (UYVY8_1X16/1920x1080) -- a real V4L2 media-controller pipeline
# validation failure, not a driver bug.
#
# Deliberately only stream 0 (mycam004m's AHD input 0) is configured here,
# not all 4 -- see NOTES.md "mycam004m real backend wired up" for why:
# only one physical camera is attached to the board as of this writing, so
# the 3-additional-route configuration needed for inputs 1-3 is genuinely
# untested (and per the same investigation, applying it changes what the
# app's already-expected doomed STREAMON attempts for the empty inputs
# touch in the shared bridge/decoder state -- not something to ship
# unverified). Revisit once more cameras are physically connected: add the
# `-R` route calls for pads 1-3 alongside a per-stream `-V` call for each,
# and confirm input 0 still streams cleanly with all 4 routes active before
# keeping the change.
#
# Safe to run even if mycam004m never probed (e.g. no camera attached) --
# the two entities below belong to the SoC-side j721e-csi2rx/cdns-csi2rx
# stack, which exists independent of whether the camera subdev bound to
# it, and media-ctl just fails (harmlessly, this being a oneshot with no
# retry) if /dev/media0 or an entity is missing entirely.
set -eu

media-ctl -d /dev/media0 -V '"cdns_csi2rx.30101000.csi-bridge":0 [fmt:UYVY8_1X16/1920x1080]'
media-ctl -d /dev/media0 -V '"30102000.ticsi2rx":0 [fmt:UYVY8_1X16/1920x1080]'
