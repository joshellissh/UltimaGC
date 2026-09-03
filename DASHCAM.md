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

**M1 — In-app recorder. DONE (2026-09-02), hardware-validated.**
Continuous capture → Wave5 → `.h264` segments on `/mnt/dvr`, from boot to power
loss. Implemented:
- **`wave5encoder.{h,cpp}`** — `Wave5Encoder` (the M0 harness logic as a class:
  resolve by caps, UYVY→H.264, capture-first STREAMON, `submit()`/`stop()`) and
  `SegmentWriter`, which **caches SPS/PPS and prepends them to every segment**
  (the encoder emits them only once, so a rotated file otherwise wouldn't
  decode) and rotates on a keyframe past the segment length so each `.h264` is
  independently playable. Qt-free; compiles standalone.
- **`dashcamrecorder.{h,cpp}`** — `DashcamRecorder` polls `/mnt/dvr` (via
  `st_dev` vs `/mnt` + `W_OK`) every 3 s and toggles `setRecording()` on all
  feeds; no drive → don't record, logged. Follows hot-plug/removal.
- **`camerafeed.{h,cpp}`** — the device now opens on display **or** recording
  (`updateOpenState()`), decoupled from the QML `active` property; the capture
  thread starts a `Wave5Encoder` **lazily on the first real frame** (so a VC
  with no camera never opens one), feeds each raw UYVY buffer, and tears down
  cleanly on stop/close. The turbojpeg spike is gone.

Recording starts a few seconds into the event loop (the recorder's poll timer),
after the boot-critical first frames — the app stays lazy at boot.

**Verified on hardware (2026-09-02)**, hot-deployed binary on the running board:
the recorder detected the drive, opened VC0, started the Wave5 encoder, and
wrote `/mnt/dvr/ULTIMA/<date>/<time>_cam1.h264` — **three clean ~61 s
rotations** (44.8 / 44.8 / 20.2 MB), no dropped frames or encoder errors. Each
segment **decodes standalone, 1525 frames, H.264 Baseline 1920×1080** (the crop
signals 1080, not the coded 1088); a *rotated* segment's leading NALs are
`SPS, PPS, AUD, IDR`, confirming the SPS/PPS-prepend that makes it independently
playable. On stop the **VPU released cleanly** (`wave5` refcount → 0, no
firmware errors), and the dash rendered normally throughout. Filenames used the
un-set RTC's date (May 2025) — the expected `unsynced/`-vs-dated behavior, real
time is M3.

**M2 — Retention janitor. DONE (2026-09-02).** The free-space ring buffer
above, implemented as `recipes-ultima/ultima-dvr-cull` (a systemd `.timer`
every 5 min firing a oneshot `ultima-dvr-cull.sh`, `Nice`/idle-I/O, separate
from `ultima-app`), wired into `IMAGE_INSTALL`. The script deletes oldest
`.h264` first under `/mnt/dvr/ULTIMA` while free space is below 10 %, reaps
emptied day dirs, and is a clean no-op with no drive mounted. Hardware-
validated: oldest-first order, threshold trigger + delete loop (with a filler
to drop free below the target), only `.h264` touched (a non-recording file was
left alone), empty dirs reaped, no-op when there's headroom; recipe builds and
packages clean. Timer *firing* confirms on the next full-image flash.

**M3 — Robustness pass. Software DONE (2026-09-02), build-validated; runtime
validation needs a full-image flash (and, for the RTC, a coin cell).**
- **exFAT fsck-on-mount — done.** exFAT has no journal and this drive is
  power-cut constantly, so `ultima-dvr-mount` now preen-fscks (`fsck.exfat -p`,
  from `exfatprogs` — added, builds) via a helper (`ultima-dvr-mount.sh`)
  launched non-blocking (`systemd-run`) from the udev rule, then mounts. Segment
  writes already `fsync` at each rotation.
- **Hot unplug — done.** `SegmentWriter::write` now closes the file on a write
  failure so a yanked drive's handle stops pinning the mount; the recorder's
  poll (which checks `W_OK`) then stops recording within ~3 s.
- **CMA — already fine** (~896 MB pool; correcting the earlier 128 MB note).
- **RTC / real filenames — software done, hardware pending.** `rtc0` is the
  onboard **DS1340** (`rtc-ds1307 2-0068`), present and in the DT, but its
  oscillator is stopped (no valid time) — it needs a **coin cell** on its backup
  connector and a **one-time set**. New `ultima-rtc-load` oneshot runs
  `hwclock -u -s -f /dev/rtc0` early at boot (before the app) and is a clean
  no-op until the RTC is valid. It replaces the excluded `ultima-hwclock-load`
  (which hardcodes a BQ32002 this board lacks). One-time set, once a battery is
  in and the system clock is correct: `hwclock -u -w -f /dev/rtc0`. Until then,
  `SegmentWriter` keeps using the interim dated-vs-`unsynced/` behavior.

Not yet on hardware: the fsck-on-mount and RTC-load only run from a flashed
image (they're boot/udev units), so validating them means a full-image build +
flash — deferred; the pieces build and package clean.

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
- **CMA pool** — *not* a concern (correcting an earlier note): this board's pool
  is ~896 MB (`CmaTotal`, per `camerafeed.h`), comfortably covering continuous
  4-cam capture + encoder buffers. No `cma=` change needed.
- **VPU teardown on exit** — the capture thread `STREAMOFF`s the encoder on a
  clean close; on process death (SIGTERM's `_exit`, or a crash) the kernel
  closes the encoder fd and the driver's `.release` runs `stop_streaming` — a
  clean teardown for a *healthy* encoder (the M0 wedge came from a failed
  seq-init blocking release, not from unclean exit), so no signal-handler
  surgery is needed. Residual risk: a crash *mid-encode* could still wedge the
  VPU until the next power-cycle — rare, and the car power-cycles every drive.
- **exFAT power-loss corruption** — no journal; fsck-on-mount + fsync + short
  segments (M3). `SegmentWriter` already `fsync`s at each rotation.
- **RTC not loaded** — bogus filename dates until the clock is valid (confirmed
  live: the board read May 2025). `SegmentWriter` routes to `unsynced/` only
  when the year is implausible (< 2021); a merely-wrong-but-set clock still gets
  a dated dir. Real fix is DS1340 hwclock-load (M3 / follow-up).
