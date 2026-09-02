# Wave5 encoder bring-up (dashcam M0)

Standalone proof that the AM67A/J722S **Wave5 VPU hardware encoder** can encode
the camera's native UYVY at 1080p25 in real time, single- and four-stream —
the de-risking step (M0) for the dashcam recorder. See `../../DASHCAM.md` for
the whole feature plan and `../NOTES.md` for the board.

This directory is a bring-up harness, not shipped in the image. The real
encoder integration is a C++ wrapper inside `ultima-app` (dashcam milestone
M1); this only answers "does the hardware do it, and how fast."

## Static findings (from the built `linux-bb.org` 6.12.43 kernel source)

Read directly out of
`drivers/media/platform/chips-media/wave5/wave5-vpu-enc.c` in the build volume
(`falcon-yocto-build`), so these are facts about *this* kernel, not upstream:

- **UYVY is a first-class raw input.** `enc_fmt_list[VPU_FMT_TYPE_RAW]` lists
  the YUV420 family, the YUV422 planar/semi-planar family, **and packed
  `YUYV`/`YVYU`/`UYVY`/`VYUY`**. UYVY maps to `FORMAT_422` + `PACKED_UYVY` in
  the driver. → **The camera's native UYVY feeds the encoder with no
  conversion** (no NEON, no GPU, no NV12 step).
- **Coded output:** `H264` and `HEVC`.
- **32 concurrent instances** (`MAX_NUM_INSTANCE`) — 4 streams is nothing.
- **Node identity:** `VIDIOC_QUERYCAP` → `driver`/`card` == `"wave5-enc"`
  (device label `"C&M Wave5 VPU encoder"`), `VFL_DIR_M2M`, **multi-planar**.
  Resolve the node by the driver name, never a fixed `/dev/videoN` (numbers are
  probe-order only — same reason `ultima-app/mediagraph.cpp` exists).
- **Colorimetry:** the encoder captures colorspace/range from the OUTPUT
  format; the cameras are **BT.601 limited range**, so tag it there.

## Dynamic bring-up findings (2026-09-02) — READ THIS before touching the encoder

Hard-won on hardware; these are load-bearing for the M1 in-app encoder:

- **Stock tools cannot drive this encoder.** `v4l2-ctl` mis-sequences the
  stateful mplane M2M device — it issues one `STREAMON`, never `DQBUF`s, and
  the CAPTURE poll times out. GStreamer on this minimal image has **no encoder
  plugins** (`v4l2h264enc`/`rawvideoparse` absent). Hence `wave5enc.c` (raw
  V4L2), which is also the reference for the app's C++ encoder.

- **STREAMON order is load-bearing: CAPTURE queue first, then OUTPUT.** The
  driver only advances to `PIC_RUN` (where it actually encodes) inside the
  OUTPUT queue's `start_streaming`, gated on `state == OPEN &&
  cap_q.streaming`. vb2 sets `q->streaming` *after* the callback returns, so
  streaming OUTPUT first leaves CAPTURE not-yet-streaming → sequence init never
  fires → the encoder silently produces nothing, **with no error logged**
  (the driver traces via `dev_dbg`). Stream CAPTURE on first.
  (`wave5-vpu-enc.c: wave5_vpu_enc_start_streaming`.)

- **A wedged VPU only clears with a cold power-cycle.** A failed/timed-out
  seq-init, or an unclean exit while streaming, leaves the firmware command
  queue stuck (`wave5_vpu_firmware_command_queue_error_check: still running:
  0x1000` spamming the log) and the instance leaked (`wave5` module refcount
  stuck at 1, a release blocked >120s in-firmware). You cannot `rmmod` at
  refcount 1, and an unbind blocks the same way. Warm `reboot` panics this
  board (Falcon). So: **cold power-cycle** to recover, and `wave5enc.c` now
  does a clean `STREAMOFF` teardown on every exit path to avoid causing this.

- **Debugging aids:** `echo "module wave5 +p" >
  /sys/kernel/debug/dynamic_debug/control` turns on the driver's traces;
  `dmesg` is empty on this image (busybox/klogctl), read the kernel log via
  `timeout N cat /dev/kmsg` instead.

Status: **PASS (2026-09-02).** On a clean VPU with capture-first ordering,
driving real camera UYVY (VC0) → Wave5 H.264: single stream **198.6 fps**
(~8× real-time), **4× concurrent 137 fps aggregate** (47–53 fps each, all
above 25 fps real-time). Output validated H.264 Baseline, 250/250 frames
decode clean, ~4 Mbit/s, byte-identical across instances. Four 1080p25
encodes fit with ~40% headroom. (Coded height is 1088 = 16-px-aligned 1080;
M1 sets the crop so the SPS signals 1080.)

## Running the dynamic test (needs the board + a live camera on VC0)

Copy `wave5-enc-test.sh` to the board and run it. It:

1. Resolves the encoder node by `driver == wave5-enc` and the CSI capture node
   by "can set UYVY 1920x1080" (override with `ENC=` / `CAP=`).
2. Captures `NFRAMES` (default 125 ≈ 5 s) of raw UYVY off the camera to
   `WORKDIR` (default `/mnt/dvr/wave5-m0` if the DVR drive is mounted, else
   `/tmp`) — unless a raw file is already there (`--reuse`).
3. Encodes it once, timed → single-stream fps + output bitrate.
4. Encodes it ×4 concurrently → aggregate fps (the real 4-camera question).
5. Leaves the `.h264` files for playback (`ffmpeg`/VLC on your Mac, or
   `ffmpeg -c copy out.mp4`).

```sh
# on the board (read-only rootfs is fine; everything lands under WORKDIR):
scp beagley-ai/wave5-enc/wave5-enc-test.sh root@ultimagc-beagley.local:/tmp/
ssh root@ultimagc-beagley.local /tmp/wave5-enc-test.sh
```

**Pass criteria:** single-stream ≥ 25 fps, and ×4 aggregate ≥ 100 fps
(4×25) — i.e. the hardware sustains four real-time 1080p streams. If ×4 falls
short, that's the signal to reconsider resolution/stream count before M1.

### Known knobs if it doesn't negotiate first try

- **Colorimetry:** if `--stream-to` fails format negotiation, the encoder is
  rejecting the OUTPUT colorimetry — set it explicitly in the `--set-fmt`
  (see the script's `ENC_OUT_FMT`).
- **Multi-planar:** `v4l2-ctl` auto-selects mplane for an M2M-mplane device;
  if a format set is rejected as single-plane, that's the culprit.
- **Board constraints:** a warm `reboot` panics this board and a module
  reload/unbind corrupts the CSI media graph — if the capture node misbehaves,
  cold power-cycle, don't reload (see the `nvp6324-bringup-workflow` memory).
