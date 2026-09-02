# camdriver — 4× AHD camera bring-up for BeagleY-AI (plan)

Bring 4 AHD camera feeds from the **MY-CAM004M** board (Nextchip **N4 / NVP6324**
AHD→MIPI-CSI2 decoder) into the gauge-cluster app, via **CSI0** on the BeagleY-AI
(AM67A / J722S). Target format: **1080p25, UYVY**, four simultaneous live feeds.

This folder holds: the kernel driver, the device-tree changes, the kernel config
fragment, the Yocto integration, and a standalone fullscreen QA app pushed over SSH.

> **Status (2026-09-01, newest): FIRST LIGHT — real decoded video on hardware; M2
> register port done + verified; framing issue remains.** The full per-channel decoder
> register sequence (LIST 1-4) is ported into `nvp6324_setup_common/_mipi/_channel/
> _channel_eq/_channel_mipi` in vendor call order (see `nvp6324-regseq-verified.md`, the
> ground-truth extraction with file:line refs). On hardware: `modprobe` → chip detect →
> `media-ctl -V` pad formats (1920x1080 UYVY on cdns + ti_csi2rx pads; required or
> STREAMON = EPIPE) → **frames flow off /dev/video2 (VC0)**. Content is REAL decoded
> video (AFE/color/format all correct) — captured, converted UYVY→PNG, recognizable
> scene. An unbind→`i2cdump`→rebind confirmed EVERY programmed register matches the
> vendor 1080p25 spec (no transcription bug); bank0 0xA0=0x0E ⇒ only CH0 has a camera.
>
> **Remaining (framing):** output is ~50 frames/s in 27ms/13ms PAIRS = 25 real
> frames/s each split into 2 CSI frames; each buffer is full 1920x1080 (no CSI errors)
> but only ~half is real content (rest green/unwritten), and the split rolls → slatted/
> sheared image. Signature of an INTERLACED source (1080i, 50 fields) fed a progressive
> 1080p25 config. Since the port faithfully matches the vendor's progressive config, the
> likely cause is the connected camera being 1080i (or otherwise not 1080p25 progressive).
> NEXT: confirm the camera's actual format; if interlaced, program the matching AHD
> format / field-weave path. CLK_AUTO was a red herring (bank1 0x7C already 0x00).
>
> **Iteration workflow (no reflash):** rebuild `.ko` (`build.sh nvp6324`) → scp to
> `/data/` → hot-reload (unbind+rmmod+insmod); ti_csi2rx then exposes a NEW /dev/videoN
> set (old ones go stale — harmless `media_device_register_entity` WARNING); stream the
> nodes `media-ctl -p` currently lists. **WARM `reboot` PANICS this board — cold
> power-cycle only.** Serial console: USB-TTL to debug header, 115200 8N1.
>
> **Status (2026-09-01, earlier): boot-safe recovery image built + flashed to the SD
> card.** The card is being re-flashed with an image that keeps the `nvp6324.ko`
> installed but **dormant at boot** — the recipe no longer sets
> `KERNEL_MODULE_AUTOLOAD` (no `modules-load.d/nvp6324.conf`) and the image bbappend
> writes `/etc/modprobe.d/nvp6324-blacklist.conf` (`blacklist nvp6324`) so udev
> coldplug won't pull it in via the DT `nextchip,nvp6324` modalias either. Verified
> via buildhistory (blacklist present, no modules-load entry, `.ko` installed). Board
> now boots to a working dash; **load the driver by hand over SSH (`modprobe nvp6324`,
> which bypasses the blacklist for an explicit by-name load) so a probe/pipeline hang
> can be watched and power-cycled — never reboot into it unattended.** Restore
> autoload + delete the blacklist once the driver is proven. (Also: the h_mask_sel
> `#define` is deliberately left at the WRONG bank0 0x8E so the current
> `setup_channel()` stub compiles; the M2 register rewrite moves it to the per-channel
> bank `0x05+ch` reg 0x38 — see `NVP6324_REG_DEC_H_MASK` and `nvp6324_regs.h`.)
>
> **Status (2026-09-01, earlier): board hung after rebooting into the fixed module —
> awaiting physical recovery.** After M1, I found two more bugs on hardware and fixed
> them (routing `NO_SINK_STREAM_MIX`; added the mandatory `V4L2_CID_LINK_FREQ`
> control the cdns bridge needs — without it stream-on fails "Unable to calculate
> link frequency / Failed to configure external DPHY -2"). I baked the fixed module
> into the card's rootfs and rebooted for a clean media graph — and the board did
> not come back (~12 min). This was the **first boot where the driver probes
> successfully and builds the CSI pipeline at boot** (the flashed image's module
> failed probe, so that path was never exercised at startup). **Lesson: do not
> autoload an unproven camera module at boot / reboot into it unattended.** Recovery:
> re-flash the card (the built image's module fails probe harmlessly → boots), or
> serial-rescue and revert /usr/lib/modules/.../nvp6324.ko. Once back: get the boot
> console/dmesg to confirm the hang cause, switch to **manual `insmod` for testing
> (drop KERNEL_MODULE_AUTOLOAD until the driver is proven)**, then port the decoder
> registers.
>
> The full AHD-1080p25 register sequence is extracted + validated and written up in
> **`nvp6324-1080p25-regseq.md`** — ready to port into `nvp6324_setup_channel()` /
> `nvp6324_setup_mipi()` (omit the test pattern; use the net-final values).
>
> **Status (2026-09-01, later): M1 DONE on hardware + full pipeline live.** Board
> booted the flashed image; `nvp6324` probes and detects the chip
> (`id 0xb0 rev 0x00`) at **i2c-4 @ 0x31** (pinmux confirmed), the CSI stack comes
> up (`cdns-csi2rx: 4/4 lanes, 4 streams, external D-PHY`), the subdev registers,
> and the media graph is complete: `nvp6324:4 → cdns_csi2rx0 → ti_csi2rx0 →
> /dev/video2..7` (6 DMA contexts). get_fmt/set_fmt (UYVY/1920x1080) and the 4 VC
> routes all work. **Fixed a probe bug** found on hardware: routing-validate must
> use `NO_SINK_STREAM_MIX`, not `NO_STREAM_MIX` (the source pad legitimately mixes
> the 4 VC streams) — else probe fails `-EINVAL`. NOTE: the *flashed card still has
> the pre-fix module*; the fix is currently only hot-loaded from /tmp (lost on
> reboot) — bake it in (rebuild + reflash, or replace /lib/modules/.../nvp6324.ko)
> before relying on a cold boot.
>
> **Next (M2 — real frames):** needs (a) AHD cameras physically connected and
> (b) the full per-channel decoder register port (the `TODO(M2)` block in
> nvp6324.c — AFE/timing/HPLL/EQ tables); the current chip setup only does
> MIPI/arbiter/VC-datatype + the 1080p25 selectors, not enough for the decoder to
> emit valid video.
>
> **Status (2026-09-01): M0 done; camera-capable image built + flashed to SD.**
> Driver compiles + MODPOSTs against the board's linux-bb.org 6.12 kernel; the
> full `tisdk-base-image` builds with the driver module, `v4l-utils`, `i2c-tools`,
> and the Falcon FIT carrying the DT overlay baked in (`fdtoverlay` in
> `ultima-falcon-fit`), and was flashed to the SD card. The board itself wasn't
> present this session, so M1+ (boot + `i2cdetect` + capture) is the next step
> once it's booted. See §10.
>
> Two resolved gotchas worth remembering:
> - **CSI0 I2C pinmux:** `main_i2c2` (the CSI0 camera bus, self-labeled
>   `hat/csi0`) has no pinmux in the BeagleY-AI base dts — the overlay muxes
>   GPMC0_CSn2/CSn3 → I2C2 (raw cells `0x00b0/0x00b4 = 0x64001`, from BeagleBoard's
>   own CSI0 IMX219 overlay). I2C address is **0x31** (bench-confirmed elsewhere;
>   still `i2cdetect`-check on first boot).
> - **Image install of the module:** IMAGE_INSTALL the recipe **PN `nvp6324`**, NOT
>   `kernel-module-nvp6324` — OE gives the versionless kernel-module provide no
>   runtime-reverse entry, so the versionless name fails `do_rootfs`. The PN
>   auto-RDEPENDS the versioned `.ko` package (the ti-img-rogue-driver pattern).

> Scope note (per CLAUDE.md): this is a **greenfield** driver. There is no NVP6324
> driver in the mainline/bb.org kernel, and we deliberately do **not** consult any
> prior removed-driver git history. The vendor reference under
> `../docs/MY-CAM004M/.../nvp6324/` is mined only for chip register sequences.

---

## 1. The headline finding — the hard part is already done in the kernel

The NVP6324 muxes all 4 analog cameras onto **one 4-lane MIPI CSI-2 link as 4 virtual
channels** (VC0–VC3), UYVY, CSI-2 data type `0x1E`. Splitting that into 4 independent
`/dev/videoN` requires **CSI2RX multi-stream / virtual-channel demux**. The concern was
that this was still under mainline review in late 2025 and might be missing from this
project's ~6.12 kernel.

**Verified directly in the build volume — it is present.** The BeagleY-AI `linux-bb.org`
kernel already ships the full stack:

- **TI SHIM** `drivers/media/platform/ti/j721e-csi2rx/j721e-csi2rx.c`:
  `TI_CSI2RX_MAX_CTX = 32`, a per-context DMA array with per-context VC select
  (`SHIM_DMACNTX_VC`, `ctx->vc`), `get_frame_desc`, `set_routing`,
  `enable_streams`/`disable_streams`, one `video_device` per context. **`num_ctx` is taken
  from the DT** — it equals the count of `dma-names` on the `ti_csi2rx` node.
- **Cadence bridge** `drivers/media/platform/cadence/cdns-csi2rx.c`: per-stream VC select
  (`CSI2RX_STREAM_DATA_CFG_VC_SELECT`), `get_frame_desc` propagation, full
  routing/streams ops (+ an `s_stream` fallback).
- Kconfig symbols all in-tree: `VIDEO_TI_J721E_CSI2RX`, `VIDEO_CADENCE_CSI2RX`,
  `PHY_CADENCE_DPHY`, `PHY_CADENCE_DPHY_RX`.

**Consequence: no kernel-core patching, no backport.** Four `/dev/videoN` come "for free"
by declaring 4 DMA contexts in the DT. Our work is (a) the NVP6324 **source subdev**, (b)
the **device tree**, (c) **config + Yocto integration**, (d) the **QA app**, (e) the
**ultima-app rebuild**.

The kernel even ships `k3-am62x-sk-csi2-v3link-fusion.dtso` (an FPD-Link fusion board:
4 cameras → VCs on one link) and the `ds90ub960` deserializer driver — **the same topology
as ours**. These are our primary DT and driver templates.

---

## 2. Hardware facts (established from datasheet + schematic + pin map; user-confirmed)

- Decoder = Nextchip **"N4" (NVP6324-class)**. Program against the **N4 register map**.
- **4-lane MIPI CSI-2**, D-PHY v1.1 (D0–D3 + CLK all wired). 4 lanes needed for 4× 1080p.
- **No host control whatsoever** (user-confirmed, corroborated by schematic): on-board
  power-on-reset, PWRDN strapped enabled, on-board **27 MHz** crystal, on-board 3.3 V/1.2 V
  regulators, **5 V from a separate input** (screw terminal / board pin), **not** the host
  connector. → The DT sensor node needs **no gpios, no clocks, no regulators**; the driver's
  power/reset hooks are **no-ops** (at most a soft I2C reset in init).
- **I2C**: single device, no EEPROM. Straps → **7-bit `0x31`** (0x62/0x63 8-bit). A
  schematic text note says `0x30` — self-contradiction. **Verify on HW** (`i2cdetect`, scan
  0x30–0x33); expect `0x31`, fall back `0x30`. No ID EEPROM → sensor declared in DT.
- **Chip ID**: bank 0, reg `0xF4 == 0xB0` (4-port), `0xF5 == 0x00` (rev). Bank-select
  register is `0xFF`; 8-bit reg / 8-bit data (regmap-friendly).
- **Pixel/bus format**: YUV422-8 / **UYVY**, CSI-2 DT `0x1E`, `MEDIA_BUS_FMT_UYVY8_1X16`
  (or `UYVY8_2X8`), 4 VCs (VC0..3) on one link.
- **Link frequency**: chip TX PLL ≈ **1.242 Gbps/lane → ~621 MHz** `link-frequencies`
  (DDR). (Sanity check: 4×1080p25 UYVY ≈ 3.3 Gbps payload; 4×567 Mbps can't carry it,
  4×1.242 Gbps can — so ~621 MHz is right.) **Confirm by DPHY lock at bring-up**, not by
  trusting either document.
- **1080p25 vs 30**: driver-forced. bank0 `0x08 ahd_mode = 0x03` (25P) / `0x02` (30P), plus
  `burst_dec_c` (bank5 `0xD1`) and `h_mask_sel` (bank0 `0x8E`). Link rate identical.
- **Cable**: MY-CAM is a 24-pin FPC, host is the 22-pin RPi-standard CSI; mapping is by
  signal name with a +1 pin offset on the lane pairs → needs the correct adapter (the
  pin-map doc is that mapping). Assumed already handled (board is plugged into CSI0).
- **Verify on HW before trusting docs**: (1) I2C address 0x31 vs 0x30; (2) SCL/SDA logic
  level 3.3 V vs 1.8 V (docs conflict; likely 3.3 V, matching the BeagleY-AI CSI I2C —
  measure before wiring, add a level shifter only if ~1.8 V).

---

## 3. Data path / architecture

```
 4× AHD cams ─▶ NVP6324 (I2C 0x31) ─ 4-lane CSI-2, VC0..3, UYVY/DT 0x1E ─▶
   dphy0 (cdns-dphy-rx) ─▶ cdns_csi2rx0 (bridge, routing/VC-select) ─▶
   ti_csi2rx0 (SHIM, 4 DMA contexts) ─▶ /dev/video0..3  (one per camera/VC)
                                             │
                                             ▼
   ultima-app CameraFeed{1..4}  ([front, rear, left, right])  +  QA app
```

The NVP6324 subdev exposes **4 streams on its source pad** (stream i ↔ VC i). Frame-desc
and enable/disable propagate NVP6324 → cdns_csi2rx0 → ti_csi2rx0; each SHIM context binds
one VC to one `/dev/video`.

---

## 4. The kernel driver (`camdriver/nvp6324.c`)

A single-file **V4L2 sub-device I2C driver**, structured like an in-tree
`drivers/media/i2c/` driver but built out-of-tree as a module.

**Model the V4L2/streams skeleton on `ds90ub960.c`** (in-tree, same "N sources → VCs on one
CSI-2 link → multiplexed streams" shape). Implement:

- `i2c_driver` + `regmap` (8-bit/8-bit). One `i2c_client` at 0x31. A helper does the
  `0xFF` bank-select with a cached current-bank + read-modify-write for sub-byte fields
  (port the `vd_register_set()` idea; drop the reference's dead `da`/cascade plumbing —
  single chip).
- `probe()`: regmap init; read `0xF4`/`0xF5`, require `0xB0`/`0x00`; register the subdev
  with `V4L2_SUBDEV_FL_STREAMS`; media entity with 1 source pad; controls (optional).
- Streams API (the multi-VC contract the CSI2RX stack calls):
  - `.get_frame_desc` → `V4L2_MBUS_FRAME_DESC_TYPE_CSI2`, **4 entries**, entry i:
    `stream = i`, `bus.csi2.vc = i`, `bus.csi2.dt = 0x1E`.
  - `.set_routing` / `v4l2_subdev_state` → 4 routes (source stream i ↔ VC i), default
    routing set at init.
  - `.enable_streams` / `.disable_streams` → ref-counted whole-chip start/stop. (The chip
    is programmed as a unit — all 4 channels configured on first enable, torn down on last
    disable; per-stream enable just ref-counts.)
  - `.get_fmt`/`.set_fmt` on the pad/stream (fixed UYVY 1920×1080; enumerate 720p later).
- `.s_power`/reset: **no-op** (hardware self-manages). Init may issue the chip's soft resets
  over I2C only (bank 0x21 `0x07` pulse, bank1 `0x80/0x81`).
- **Chip programming ported from the reference** (chip-only files, not the Allwinner glue):
  - `jaguar1_mipi.c` → `mipi_tx_init` (PHY/PLL + datatype 0x1E), `arb_init`,
    `mipi_video_format_set`, `disable_parallel`.
  - `jaguar1_video.c` → per-channel decoder seq1–seq9 (AFE, format, timing, H-scaler, HPLL,
    color, clocks).
  - `mipi_dev_nvp6324.c` → `video_decoder_init`, `vd_set_all`, `nvp6324_init` orchestration.
  - Tables: `jaguar1_video_table.h`, `jaguar1_mipi_table.h`, `jaguar1_reg_set_def.h`, cableA
    EQ table. Fold the needed values in as static register arrays; force **AHD20_1080P_25P**
    (`ahd_mode=0x03`) on all 4 channels.
  - Skip the coax/PTZ protocol (not needed for video) and the dead char-driver/`i2c/` paths.
- **VC-assignment register**: the reference writes `chid_vin = 0x00` for every channel, so
  the ch→VC map is not pinned in the code — **confirm VC0..3 ↔ ch map from the N4 datasheet**
  and set it explicitly so each camera lands on its intended VC.

Structure suggestion: `nvp6324.c` (driver + V4L2) + `nvp6324_regs.h` (ported register
sequences/tables) to keep the chip data separate from the V4L2 plumbing.

---

## 5. Device tree (`camdriver/dts/`)

Add on top of `k3-am67a-beagley-ai.dts`, modeled on the IMX219 CSI0 overlay + the
v3link-fusion DTSO:

- `&nvp6324` sensor node on the **CSI0 I2C bus** at **0x31**, one `port`/`endpoint`:
  `data-lanes = <1 2 3 4>`, `clock-lanes = <0>`,
  `link-frequencies = /bits/ 64 <621000000>` (confirm), remote-endpoint → `cdns_csi2rx0`
  input. **No gpios/clocks/regulators.**
- `&cdns_csi2rx0` input endpoint back-links the sensor; `status = "okay"`.
- `&dphy0` `status = "okay"`.
- `&ti_csi2rx0`: `status = "okay"` and **four DMA channels + `dma-names`** (e.g. names for
  4 contexts) so `num_ctx = 4` → four `/dev/videoN`. (Check what CSI0's `ti_csi2rx0`
  provides in the base dtsi; extend to 4 the way the v3link-fusion/quad examples do.)

**Delivery — no runtime overlay.** This project boots via **Falcon** (R5 SPL → static DTB
in the FIT); there is **no U-Boot overlay-apply stage**. So the camera DT ships as a
**unified-diff patch to the board dts** (exactly the `0001-...enable-usb1-host.patch`
pattern), baked into the FIT. Bench iteration can still use `fdtoverlay` on the deployed
DTB + SSH push (as the USB1 fix was validated) before baking in.

---

## 6. Kernel config fragment (`camdriver/kernel/nvp6324.cfg`)

Merged via `KERNEL_CONFIG_FRAGMENTS` (same mechanism as `ultima-boot.cfg`). Ensure:

```
CONFIG_MEDIA_SUPPORT=y
CONFIG_V4L_PLATFORM_DRIVERS=y
CONFIG_VIDEO_CADENCE_CSI2RX=m
CONFIG_VIDEO_TI_J721E_CSI2RX=m
CONFIG_PHY_CADENCE_DPHY_RX=m
# (our NVP6324 module is built by its own out-of-tree recipe)
```

Build the camera stack as **modules (`=m`)**, not built-in: this repo counts boot
initcalls, and udev coldplug is already ordered *after* the app's first frame, so late
module load is free. (Confirm the exact current values against the running defconfig; only
add what's missing.)

---

## 7. Yocto integration (`beagley-ai/meta-ultima-beagley-ai-src/`)

- **Out-of-tree module recipe** `recipes-kernel/nvp6324/nvp6324.bb` (`inherit module`),
  building `camdriver/` (driver + Makefile). Follow the ultima-app source model:
  bind-mount `camdriver/` read-only into the container, copy into WORKDIR, build against the
  kernel (with the `do_unpack`/`do_configure` `nostamp` lesson from ultima-app.bb so source
  edits actually rebuild). Autoload via DT `compatible` match at coldplug (or
  `modules-load.d` if needed).
- Wire it into `beagley-ai/build.sh`'s bind-mounts and add to the image (`IMAGE_INSTALL`).
- `linux-bb.org_%.bbappend`: add `SRC_URI:append` for the **dts patch** and the
  **`nvp6324.cfg`** fragment (mirrors the existing USB1 patch + boot-cfg wiring).

---

## 8. QA app (`camdriver/qa/` — pushed over SSH, not baked into the image)

A **plain C** fullscreen 2×2 viewer — deliberately minimal, decoupled from ultima-app so it
tests the driver, not the gauge cluster:

- **V4L2 mmap capture** on `/dev/video0..3` (UYVY, `V4L2_MEMORY_MMAP`), 1080p25.
- **DRM/KMS dumb buffer** display (tidss), single fullscreen framebuffer, 4 quadrants;
  CPU UYVY→XRGB8888 blit into each quadrant (correctness over speed — this is QA).
- Must **stop `ultima-app.service`** first (nothing else may own the display).
- Deps: libdrm only; cross-compile with the container toolchain, `scp` the static-ish
  binary. No Qt, no GStreamer.
- **Per-channel lock/status readout** (from the NVP6324 no-video status registers, or at
  least a "frames arriving y/n" per node). Rationale: with format forced to 1080p25, a camera
  set to NTSC/30 (or a dead input) produces a **black quadrant with no other symptom** — a
  lock readout saves a bench day of blind debugging.
- Optional: a `--single N` mode to view one node, and a text-only `--probe` mode (chip ID +
  per-channel lock) for headless SSH checks.

(Later, `v4l2-ctl`/`media-ctl` from `v4l-utils` cover the same ground for spot checks —
confirm `v4l-utils` is in the image or push a static build.)

---

## 9. ultima-app CameraFeed rebuild (separate milestone, board-agnostic)

The app's old V4L2/zero-copy pipeline was removed with the prior driver; `CameraFeed` is a
placeholder today (see `GAUGE-CLUSTER.md`). Rebuild it against the real driver — its own
milestone with a stated acceptance contract:

- 4× `/dev/videoN`, **UYVY**, **25 fps**, `VIDIOC_EXPBUF` (dma-buf) works.
- Zero-copy: dma-buf → EGLImage `DRM_FORMAT_UYVY` → `GL_TEXTURE_EXTERNAL_OES`; NEON
  UYVY→RGBA half-res for the SurroundView stitch path.
- Request CPU-cacheable buffers — **check the j721e-csi2rx vb2 queue advertises
  `V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS`** (`V4L2_MEMORY_FLAG_NON_COHERENT`); the stitch
  path's cached-read fix depends on it (it's a small kernel patch if absent).
- Map `cameraFeed1..4` = **[front, rear, left, right]** to the right VCs/nodes.

---

## 10. Milestones (staged; hardware-gated)

- **M0 — Mac/container only (no board):** scaffold `camdriver/`; driver skeleton (probe +
  chip-ID + streams stubs) **cross-compiles as a module** against the kernel; dts patch
  compiles (dtc); config fragment lands; Yocto recipe builds an image.
- **M1 — board, I2C:** `i2cdetect` finds the chip (settles 0x31 vs 0x30); driver **probes**
  and reads `0xF4 == 0xB0`. Also settles the SCL/SDA level question.
- **M2 — board, one camera:** `media-ctl` links the pipe; `v4l2-ctl --stream-mmap` on
  `video0` (VC0/front) yields UYVY frames. **Proves DPHY lock, link-frequency, format** on
  one camera — independent of the multi-VC question.
- **M3 — board, four cameras:** all `video0..3` stream **simultaneously**, one per camera.
  Validates routing / frame-desc / 4 DMA contexts end to end.
- **M4 — QA app:** fullscreen 2×2 + per-channel lock readout, over SSH.
- **M5 — ultima-app:** rebuild `CameraFeed`, zero-copy, wire the 4 feeds; grid/rear/360
  screens live.

---

## 11. Guardrails (from CLAUDE.md / NOTES.md)

- **Do not** consult prior removed-driver git history; the vendor reference is the only
  bootstrap source.
- CAN1 is untouchable; this task doesn't go near CAN.
- Rootfs is read-only; for live hand-edits `mount -o remount,rw /` and power off immediately
  if anything crash-loops (an earlier crash-loop caused real rootfs I/O errors).
- Falcon boot: no interactive bootloader, static baked DTB — DT ships as a patch, not a
  runtime overlay.
- Commits in this repo: **no `Co-Authored-By`/`Claude-Session` trailers**.
- Target hardware isn't always attached — **ask before assuming** flash/CAN-sniff access; M0
  is fully doable Mac-only.

---

## 12. Open items to confirm on real hardware

1. ~~I2C address~~ — **resolved: `0x31`.** Strap analysis (SA0=H/SA1=L) and an
   independently-found bench note in the checkout agree; still worth an `i2cdetect`
   sanity check on first power-on.
2. SCL/SDA logic level (expect 3.3 V; measure; level-shift only if 1.8 V).
3. Exact MIPI `link-frequencies` (via DPHY lock at M2; ~621 MHz expected).
4. VC0..3 ↔ camera-channel mapping (from N4 datasheet; set explicitly).
5. ~~`ti_csi2rx0` base DMA contexts~~ — **resolved: the base dtsi already gives
   `ti_csi2rx0` six DMA channels (`dma-names = "rx0".."rx5"`)**, so enabling it
   yields up to 6 `/dev/videoN` with no DMA additions; we use 4. The overlay just
   enables the nodes + wires the sensor endpoint.
6. `V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS` on the SHIM's vb2 queue (for M5's cached-read).
