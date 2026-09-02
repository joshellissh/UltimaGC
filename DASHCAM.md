# Ultima Dashcam — Design & Implementation Plan

Continuous multi-camera dash-cam recording: from the moment the board boots
until it loses power, encode every connected camera stream to an auto-mounted
USB drive, with human-readable files and automatic old-footage culling.

**Status (2026-09-02): planned, not yet implemented.** The storage plumbing
(USB automount) is done and shipping; the recording pipeline itself is the
work this document describes. This is the agreed design, captured as the
durable reference — read it alongside `beagley-ai/NOTES.md` (board/boot/kernel,
the Wave5 probe log, the NVP6324 CSI-2 pipeline) and `GAUGE-CLUSTER.md` (the Qt
app structure and the existing camera capture pipeline this builds on).

This feature is the entire reason for the BeagleY-AI (AM67A / J722S) port: its
**Wave5 hardware video encoder** is what makes 4-camera H.264 recording
possible at all (see `beagley-ai/NOTES.md`, "The reason for this board").

## Decisions (settled)

| Decision | Choice | Notes |
|---|---|---|
| Camera target | **4× 1080p25** | Gated on the 1242 Mbps MIPI eye — see M4. Ships incrementally on the 1 proven camera first. |
| Where recording lives | **Inside `ultima-app`** | The app is already the sole, stable owner of the cameras with a mature zero-copy path. |
| File format | **Raw H.264 Annex-B segments (`.h264`)** | Most robust to abrupt power loss: no muxer, no `moov` atom to lose; a power cut only truncates the tail. Remux to MP4 offline with `ffmpeg -c copy`. |
| Retention | **Free-space ring buffer** | A janitor deletes oldest segments first once the drive nears full; adapts to any stick size. |

## Why in-app, not a separate recorder service

The tempting alternative — an independent GStreamer service owning the cameras
(`v4l2h264enc ! splitmuxsink`, which gives mux + segmentation + culling nearly
for free) — was **rejected**, for reasons specific to this project:

- **V4L2 capture is single-consumer.** On the J722S CSI2RX stack each virtual
  channel maps to one SHIM context = one `/dev/videoN` capture node, and a
  capture node allows a single streaming open. A separate recorder owning the
  cameras would force the app's camera *screens* (360 view, rear/reverse,
  blind-spot overlays, grid) to get their frames some other way — cross-process
  dma-buf IPC (real work) or a v4l2loopback tee (~400 MB/s of raw 1080p memcpy
  across 4 streams, which throws away the zero-copy path that took real effort
  to build — see `GAUGE-CLUSTER.md`, "Camera framerate: root cause found").
- **The reliability argument for a separate process is weak here.** `ultima-app`
  has an `NRestarts=0` track record on hardware and restarts in ~1.2 s; and if
  the app is down, the whole dash is dark anyway — a recording gap in that
  window is the least of the problems.
- **Image ethos.** This image is deliberately minimal and boot-time-optimized
  (see the generator/unit trimming in the image bbappend). Adding the full
  GStreamer stack cuts against that.

So the app stays the single camera owner, and recording is added as another
**consumer of the existing capture buffers**, driving the Wave5 encoder
directly through its V4L2 mem2mem interface (no GStreamer).

## Data flow

```
NVP6324 → CSI2RX → /dev/videoN  (UYVY 1080p25, per virtual channel; app owns)
   │
   ├─► [display]  existing zero-copy dma-buf → GPU external texture   (unchanged, on-demand)
   │
   └─► [record]   Wave5 M2M encode (UYVY in, no convert)  →  H.264 Annex-B
                  (/dev/videoM by caps, hardware H.264,          segmented, cut on
                   one session/cam; capture dma-buf              IDR boundary → file
                   imported directly — zero-copy)               on /mnt/dvr
```

**The encoder accepts the camera's native UYVY directly** (confirmed in
M0 — see below), so recording needs *no* pixel conversion. The record path
lends each capture buffer to the encoder's OUTPUT queue by dma-buf import
(`V4L2_MEMORY_DMABUF`) — the buffers are already exported for the display
zero-copy path — so the CPU never touches recorded pixels. The buffer returns
to the capture queue once the encoder is done with it, arbitrated by the same
buffer-lending refcount the display path already uses
(`GAUGE-CLUSTER.md`). This sits where the (now-removed) turbojpeg spike was in
`CameraCaptureThread::run()`.

## What already exists (do not rebuild)

- **USB automount — done and shipping.** `recipes-ultima/ultima-dvr-mount`
  (in `IMAGE_INSTALL`): a udev rule mounts any block device labeled
  `ULTIMA_DVR` to `/mnt/dvr` via `systemd-mount`
  (`noatime,nosuid,nodev,noexec`), and unmounts on removal. exFAT, for
  cross-platform browsability. No auto-format — the drive is prepared by hand
  (`mkfs.exfat -n ULTIMA_DVR`). It is the only removable disk on the system,
  so matching on LABEL alone is unambiguous.
- **Camera capture — mature.** `nvp6324` driver → Cadence/TI CSI2RX,
  `nvp6324-csi-setup` boot oneshot propagates the 1080p format down the
  pipeline, and the app resolves the capture node per virtual channel through
  the media graph (`ultima-app/mediagraph.{h,cpp}`) rather than a fixed
  `/dev/videoN`. VC0 = one camera, 1080p25 UYVY, hardware-proven (CRC=0). See
  `beagley-ai/NOTES.md` and the `nvp6324-*` memory notes.
- **Wave5 encoder — present, probed, never driven.** The kernel logged it at
  first boot: `wave5 driver with caps: 'ENCODE' 'DECODE'`, product code
  `0x521c`. Node map on this SoC: `video0` = VPU decoder, `video1` = VPU
  encoder, `video2..7` = CSI capture contexts (VC0 = `video2`). Resolve the
  encoder by `VIDIOC_QUERYCAP` name/caps, **not** a hardcoded `/dev/video1` —
  same discipline `mediagraph.h` already applies to capture (the numbers are
  probe-order only).

### To be removed (dead — old-board carryover)

The prior encode spikes were measured on the **previous board (BeaglePlay /
AM625, which has no hardware video encoder)**, where software was the only
option, and were carried into this repo. Both are irrelevant now and get
deleted:

- **openh264** (software H.264): ~233 ms/frame at 1080p, ~3.5 fps — reverted.
- **turbojpeg** (software MJPEG): ~67 ms/frame — kept only as a measurement
  spike inside `camerafeed.cpp` (`ULTIMA_CAM_RECORD_FPS` / `ULTIMA_CAM_RECORD_DIR`).

Remove the spike, the `-lturbojpeg` link (`ultima-app.pro`), the
`libjpeg-turbo` DEPENDS/RDEPENDS (`ultima-app.bb`), and
`deinterleaveUYVYto422()` — M0 confirmed the encoder takes UYVY directly, so
even the deinterleave helper is dead. Nothing from the spike is reused.

## Wave5 encoder facts (confirmed in M0 against the built kernel source)

Driven through the standard **V4L2 mem2mem** stateful-encoder interface, the
multi-planar API (the driver works in `v4l2_format.fmt.pix_mp`). Read straight
from `drivers/media/platform/chips-media/wave5/wave5-vpu-enc.c` in this build's
`linux-bb.org` 6.12.43 kernel-source:

- **Raw input formats** (`enc_fmt_list[VPU_FMT_TYPE_RAW]`): the full YUV420
  family (`NV12`, `YUV420`, `NV21`, and `*M` multi-plane variants), the YUV422
  planar/semi-planar family (`YUV422P`, `NV16`, `NV61`, …), **and packed 4:2:2
  `YUYV` / `YVYU` / `UYVY` / `VYUY`**. For `UYVY` the driver sets
  `src_format = FORMAT_422` and `packed_format = PACKED_UYVY` — genuine
  hardware handling, not a stub. **So the camera's native UYVY feeds the
  encoder with zero conversion; no NEON, no GPU, no color-converter.**
- **Coded output** (`enc_fmt_list[VPU_FMT_TYPE_CODEC]`): `H264` and `HEVC`.
- **Concurrency:** `MAX_NUM_INSTANCE = 32` — four simultaneous 1080p sessions
  is far within limits (each `open()` is one instance).
- **Node identity:** `VIDIOC_QUERYCAP` returns `driver`/`card` = `"wave5-enc"`
  (device label `"C&M Wave5 VPU encoder"`), `vfl_dir = VFL_DIR_M2M`. Resolve
  the node by this, never a hardcoded `/dev/video1`.
- **Colorimetry:** the encoder captures `colorspace` / `ycbcr_enc` /
  `quantization` / `xfer_func` from the OUTPUT format. The cameras emit
  **BT.601 limited range** (see `GAUGE-CLUSTER.md`), so tag the OUTPUT format
  accordingly — the V4L2 encoder path is otherwise picky about colorimetry
  negotiation.
- **STREAMON order is load-bearing — CAPTURE queue first, then OUTPUT.** The
  driver reaches its encoding state (`PIC_RUN`) only inside the OUTPUT queue's
  `start_streaming`, and only if the CAPTURE queue is *already* streaming; vb2
  sets `q->streaming` after the callback returns, so OUTPUT-first leaves the
  encoder silently emitting nothing, with **no error logged**. Both stock tools
  get this wrong (see `beagley-ai/wave5-enc/README.md`), which is why the app
  drives the M2M device itself. An unclean exit while streaming, or a failed
  seq-init, **wedges the VPU firmware** — recoverable only by a cold
  power-cycle — so the record path must always `STREAMOFF` cleanly.

## Segmentation & filenames

Raw Annex-B, cut on an **IDR boundary** so every segment is independently
decodable: configure the encoder GOP/IDR period to align with the segment
length, and start a new file at the first IDR NAL after the segment duration
elapses (default target **60 s** segments — short enough that a power cut loses
little, long enough to keep file counts sane).

```
/mnt/dvr/ULTIMA/
  2026-09-02/                     # local date; sorts chronologically
    14-30-00_cam0.h264            # segment start time + camera label
    14-31-00_cam0.h264
    14-30-00_cam1.h264            # per-camera, same time buckets
  unsynced/                       # written here while the wall clock is invalid
    boot-000/                     #   (no RTC-set time yet — see risks)
      000000_cam0.h264
```

Playback: VLC / ffmpeg directly. `ffmpeg -c copy out.mp4` remuxes to MP4
offline for anyone who wants double-click playback.

## Retention janitor (free-space ring buffer)

A **separate** small recipe (e.g. `ultima-dvr-cull`): a script driven by a
systemd timer, deliberately *not* part of `ultima-app` so neither can take the
other down. Every few minutes: while free space on `/mnt/dvr` is below a
threshold (e.g. keep ≥ 10 % free), delete the oldest segment (files/day-dirs
sort chronologically by name). Deletes empty day directories as they drain.
Adapts to any drive size with no per-drive configuration.

Storage math for sizing: H.264 1080p25 ≈ 4 Mbps ≈ **~1.8 GB/hr per camera**
(~7 GB/hr for four). A 256 GB stick ≈ ~140 hr single-cam, ~35 hr four-cam.

## Milestones

Sequenced so a working recorder ships on the one proven camera **before** the
4-camera hardware signal-integrity work — 4×1080p is the target, not a blocker
on the whole feature.

**M0 — Wave5 encode proof (standalone, before touching the app).** De-risk the
encoder in isolation, respecting the board's hard constraints (a warm `reboot`
panics this board; a module reload / unbind corrupts the media graph — see the
`nvp6324-bringup-workflow` memory).
- **Static analysis — DONE (2026-09-02).** Grepped the built `linux-bb.org`
  6.12.43 wave5 source in the volume: UYVY is a first-class raw input (zero
  conversion), 32-instance limit, node resolves by `driver == "wave5-enc"`,
  multi-planar API, BT.601 tagging via the OUTPUT format. See "Wave5 encoder
  facts" above.
- **Dynamic proof — DONE (2026-09-02), PASS with large margin.** Stock tooling
  couldn't drive the encoder (v4l2-ctl mis-sequences the stateful mplane M2M
  device; GStreamer on this minimal image has no encoder plugins), so
  `beagley-ai/wave5-enc/wave5enc.c` is a small raw-V4L2 M2M harness
  (cross-compiled static aarch64, and the M1 encoder reference). Driving real
  camera UYVY (VC0) → Wave5 H.264 on the board:
  - **single stream 198.6 fps** (250 frames / 1.26 s) — ~8× real-time;
  - **4× concurrent 137 fps aggregate** (each stream 47–53 fps) — every stream
    well above 25 fps real-time, ~40% headroom on four 1080p streams;
  - output validated: H.264 Baseline, 250/250 frames decode clean, ~4 Mbit/s,
    byte-identical across all five instances (deterministic).

  So **four simultaneous 1080p25 encodes are comfortably within the hardware** —
  the board's whole premise, confirmed. Two facts carried into M1: the
  capture-before-output STREAMON ordering (above), and set the encoder crop
  (`conf_win`) so the SPS signals 1080 display height (the encoder codes
  1088, the 16-px-aligned height). Full bring-up story, incl. the VPU-wedge
  recovery and debugging aids, in `wave5-enc/README.md`.

**M1 — Single-camera in-app recorder.** Continuous capture → convert → Wave5 →
`.h264` segments on `/mnt/dvr`, from boot to power loss, proven on today's
clean 1×1080p25 VC0. Recording holds a permanent frame-consumer reference so
its camera stays streaming regardless of what's on screen (display screens add
transient references on top). Service ordering gates on the mount
(`RequiresMountsFor=/mnt/dvr`); no drive present = don't record, log it (and,
later, surface a dash indicator).

**M2 — Retention janitor.** The `ultima-dvr-cull` timer/script above.

**M3 — Robustness pass.**
- exFAT has **no journal**: abrupt power loss can corrupt the *filesystem*, not
  just the tail segment. Add an `fsck.exfat` before mount (confirm
  `exfatprogs` is in the image — the current udev `systemd-mount` does no
  fsck), `fsync` at each segment close, and keep segments short.
- Hot unplug / replug handling.
- **CMA sizing.** Continuous 4-camera capture + 4 encoder sessions (each with
  reference frames + IO buffers) on top of the ~96 MB the display EGLImages
  already pin will exceed the 128 MB CMA pool. Measure, then raise `cma=`.
  Note: bootargs are baked into the Falcon FIT now, so this is a **rebuild**,
  not a live cmdline edit (see `beagley-ai/NOTES.md`).
- **Clock / filenames.** The onboard DS1340 RTC exists but `ultima-hwclock-load`
  isn't installed, so recordings before a valid time-set get a bogus epoch
  date. Interim: use the app's existing `SystemClock::timeIsValid()` to write to
  `unsynced/boot-N/` until the clock is good. Follow-up: bring up an
  hwclock-load variant for the DS1340 so timestamps are real from boot.

**M4 — Scale to 4×1080p (hardware prerequisite for the target).** The
NVP6324's 1242 Mbps "FHD x4ch" MIPI rate is a **marginal eye** on this board
today (CRC errors + shear per the `nvp6324-arbiter-vc-mask` memory); 594 Mbps
is the proven-clean rate but only carries 1×1080p or ~4×720p. Four clean 1080p
streams is board-level signal-integrity work (link timing block / PLL /
termination / `vc_mask` for the populated channels), separate from the recorder
itself. Because M1–M3 are written for N streams, this milestone is "make the
pixels clean + open 4 encoder sessions + per-camera directories," not a
rewrite.

## Open risks, in one place

- **MIPI 1242 Mbps eye** — the gate on 4×1080p (M4).
- **CMA pool** — needs raising; a Falcon-FIT rebuild (M3).
- **exFAT power-loss corruption** — no journal; fsck-on-mount + fsync + short
  segments (M3).
- **RTC not loaded** — bogus filename dates until the clock is valid; interim
  `unsynced/` dir, real fix is DS1340 hwclock-load (M3 / follow-up).
