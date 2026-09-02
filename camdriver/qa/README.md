# camqa — camera driver QA tool (BeagleY-AI, AM67A/J722S)

A tiny, dependency-light C program for **manually** bench-testing the NVP6324
(MY-CAM004M) 4-channel AHD → MIPI-CSI2 camera path. The kernel CSI2RX stack
demuxes the four virtual channels into four V4L2 capture nodes
`/dev/video0..3`, each streaming **UYVY, 1920×1080, ~25 fps**.

`camqa` captures all four and paints them as a **fullscreen 2×2 grid** on the
`tidss` DRM display, using a single dumb framebuffer and a CPU UYVY→XRGB8888
blit. It is pushed to the board over SSH and run by hand — it is **not** baked
into the image.

Dependencies: **libc + V4L2 + libdrm only**. No Qt, GStreamer, X11, or Wayland.

## Why it exists (the diagnostic point)

The driver forces every channel to 1080p25. A camera set to a different
standard (NTSC/30, PAL, …) or an unplugged input therefore produces a **black
quadrant with no other symptom**. So `camqa`'s whole reason for being is the
explicit per-channel readout: a live `videoN: 25.0 fps` / `videoN: NO SIGNAL`
line, plus a green (receiving) / red (no frames in the last second) quadrant
border and a small on-screen `N NO SIG` label. If a quadrant is black, the
readout tells you instantly whether frames are arriving.

## Files

- `camqa.c` — the whole app (single file: `drm_init`, `cam_open`,
  `cam_drain_newest`, `uyvy_to_xrgb_scaled`, `run_display`, `run_probe`, …).
- `Makefile` — cross-compile friendly (`CROSS_COMPILE` / `CC` / `CFLAGS` /
  `SYSROOT`, links `-ldrm`).

## Building (cross-compile against the Yocto SDK/sysroot)

### Option A — a populated SDK (recommended for a clean workstation)

Build an SDK once from the same Yocto config that builds the image, then source
its environment and `make`:

```sh
# inside the build container (see ../../beagley-ai/run.sh):
bitbake -c populate_sdk tisdk-base-image        # or: meta-toolchain

# install + source the generated environment (path/name will match your SDK):
. /opt/ti/<version>/environment-setup-aarch64-oe-linux
make                                            # CC/CFLAGS/SYSROOT come from the env
```

The `environment-setup-*` script exports `CC`, `CFLAGS`, `PKG_CONFIG_*`, etc.
The Makefile uses `pkg-config --cflags --libs libdrm` when it is available (the
SDK env sets it up) and honors the environment `CC` (origin *environment*,
which correctly overrides make's built-in default).

### Option B — build against an in-tree recipe-sysroot (no SDK install)

The project's Docker build volume already contains a target sysroot with libdrm
and the kernel UAPI headers (any recipe that depends on libdrm — e.g.
`ultima-app` — has one), plus an `aarch64-oe-linux-` cross gcc. This is the
fastest path on this Mac and is exactly how the binary was verified. From
`camdriver/qa/`:

```sh
docker run --rm \
  -v falcon-yocto-build:/y \
  -v "$PWD:/src:ro" \
  falcon-yocto:latest bash -c '
    B=/y/tisdk/build/arago-tmp-default-glibc/work/aarch64-oe-linux/ultima-app/1.0
    cp -r /src /tmp/qa && cd /tmp/qa
    make \
      CC="$B/recipe-sysroot-native/usr/bin/aarch64-oe-linux/aarch64-oe-linux-gcc" \
      SYSROOT="$B/recipe-sysroot"
    cp camqa /src-out/ 2>/dev/null || true
    file camqa
  '
```

(To get the binary back out, add `-v "$PWD:/src-out"` and drop the `:ro`, or
`docker cp` from a non-`--rm` run.) When `pkg-config` is not on `PATH` the
Makefile falls back to `-I$(SYSROOT)/usr/include/libdrm -ldrm`, which resolves
the same headers — both paths were confirmed to build an identical aarch64 ELF.

### Manual invocation

```sh
make CROSS_COMPILE=aarch64-oe-linux- SYSROOT=/path/to/recipe-sysroot
# or
make CC=aarch64-oe-linux-gcc SYSROOT=/path/to/recipe-sysroot
```

> **Verification status:** the tool has been cross-compiled clean
> (`-Wall -Wextra`, no warnings) into a valid `ELF aarch64` binary against the
> target sysroot, and the DRM symbols resolve against `libdrm.so.2`. It has
> **not** yet been run on real hardware — that is the final verification step
> (see "What to confirm on hardware" below).

## Deploying and running

The board's **rootfs is mounted read-only**, so copy the binary to a writable
location — `/data` (the persistent partition) or `/tmp` (RAM):

```sh
scp camqa root@<board-ip>:/data/camqa            # or :/tmp/camqa
ssh root@<board-ip>
chmod +x /data/camqa
```

### 1. Headless sanity check first (no display, zero risk)

`--probe` never touches DRM, so it does **not** require stopping the cluster —
it is the safe first command on a live board. It streams briefly and prints a
per-node status to stdout:

```sh
/data/camqa --probe
```

Example output:

```
video0: OK, 74 frames total
video1: OK, 75 frames total
video2: NO SIGNAL (0 frames)
video3: OK, 73 frames total
```

### 2. Fullscreen grid (needs the display)

The display can only have one owner. `ultima-app.service` holds it, so stop it
first, run the grid, then restart it when done:

```sh
systemctl stop ultima-app.service
/data/camqa                      # 2x2 grid; per-node fps to stderr; Ctrl-C to quit
systemctl start ultima-app.service
```

If `camqa` prints `another process owns the display`, the service is still
running — stop it and retry.

## Options

```
--single N         Show only /dev/videoN, fullscreen (not the grid).
--probe            Headless: stream briefly, print per-node status, exit. No DRM.
--device PATH      DRM device (default /dev/dri/card0).
--count N          Number of nodes /dev/video0..N-1 (default 4).
--width N          Capture width override  (default 1920).
--height N         Capture height override (default 1080).
--help             Usage.
```

## Behavior notes / design choices

- **A dead node never blocks the others.** Nodes are opened `O_NONBLOCK`; the
  main loop `poll()`s across all fds and a node with no signal simply produces
  no `POLLIN`. A node that fails to open is still drawn as a red "NO SIG"
  quadrant rather than shrinking the grid.
- **Newest-frame-wins.** On each wakeup the tool drains a node's whole backlog,
  counts every frame for the fps stat, but blits only the newest — a CPU blit
  at 4×1080p25 can fall behind real time, and this keeps latency bounded.
- **No stale frames.** When a node goes quiet its quadrant is cleared to dark
  and re-labelled NO SIG — a frozen last frame that looks alive would defeat
  the tool.
- **Format is read back, not assumed.** `camqa` honors the driver's returned
  `bytesperline`/`sizeimage` and buffer count, and warns if the negotiated
  fourcc is not UYVY.
- **Colors:** BT.601 *limited-range* YUV→RGB (what these AHD cameras emit).
- **Clean teardown** on SIGINT/SIGTERM: STREAMOFF, munmap, close, RmFB, destroy
  dumb buffer, drop DRM master. The previous CRTC is *attempt*-restored; because
  stopping `ultima-app` already freed its framebuffer, that restore usually
  fails harmlessly and the last captured frame is simply left on screen.

## Assumptions (worth confirming on hardware)

- **DRM device / connector:** defaults to `/dev/dri/card0` and picks the first
  **connected** connector with modes, its preferred mode, and a compatible
  CRTC. On this board `tidss` is `card0`; the PowerVR GPU can enumerate as a
  second DRM node with no connectors (hence the "not a KMS card" guard and the
  `--device` override).
- **Quadrant order** is reading-order: `0`=top-left, `1`=top-right,
  `2`=bottom-left, `3`=bottom-right. This is *not* the app's
  `[front, rear, left, right]` feed mapping — it is just node index order, so a
  channel can be identified by its label.
- **Format** requested is UYVY / 1920×1080 / field NONE, 4 MMAP buffers.
- Terminal modes are never altered (no raw mode), so there is nothing to
  restore beyond flushing output.
