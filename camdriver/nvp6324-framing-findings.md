# NVP6324 framing bug — empirical findings (2026-09-01)

Live capture of `/dev/video2` (VC0, the one connected AHD camera), raw UYVY 1920×1080
buffers dumped from inside camqa's stable continuous stream (`--dump`), analysed
offline (scratchpad `analyze.py`/`analyze2.py`).

## Hard facts

1. **Camera standard is 25p, not 30p.**
   - `ahd_mode=3` (AHD20_1080P_25P): raw frame is **real scene content** (a landscape),
     full 1920 width, coherent — but sheared (see #3).
   - `ahd_mode=2` (1080p30): adjacent rows are **decorrelated noise** (FFT row-to-row
     shift ≈ −150 px/line). 30p does NOT lock. → keep 25p.

2. **Only the top 720 of 1080 lines carry video; rows 720–1079 are exactly zero**
   (luma std = 0.0, mean = 0.0 — unwritten, not garbage). 720 = exactly ⅔ of 1080.
   Stable frame-to-frame (user sees "top 2/3" consistently), so MIPI frame-sync (FS/FE)
   IS working — the frame genuinely has 720 active lines and the receiver zero-pads to
   1080.

3. **Horizontal shear ≈ +2.2 px/line** (adjacent-row FFT cross-correlation, core mean
   +2.23 px/line over the 720 valid rows; ≈1600 px cumulative drift). This is what turns
   the scene into diagonal "marbled" flowing bands. 2 px = 4 bytes/line.

4. **bytesused is a full 4147200 every frame** in camqa's continuous stream (both modes,
   partial=0). The earlier v4l2-ctl start/stop runs that showed 1613824 / <1024 / 0-byte
   "runt" frames were a **nondeterministic STREAMON lock race** (each stream-on reprograms
   the whole chip against an already-armed cdns/ti receiver), NOT a real per-mode
   property. Do not capture via repeated v4l2-ctl start/stop; dump from inside the live
   stream instead.

5. Capture rate ≈ **50 fps** (2× the 25 fps real rate) measured by `camqa --probe`.
   720 active lines/frame + higher-than-25 frame rate is consistent with the chip
   emitting a **720-line raster** rather than 1080.

## Working hypothesis

The NVP6324 MIPI-TX is emitting a raster that is **720 lines tall with a per-line
horizontal length a couple px off** from what the cdns/ti receiver is programmed for
(1920×1080). Both defects are transmit-side raster geometry, not analog decode (the
analog content is clearly correct at 25p). Suspect the vendor MIPI-TX / video-length
registers for AHD20_1080P_25P — a line-count / horizontal-total ("VVL/VVH"-style)
setting either mis-ported or not ported.

MIPI-TX table in the driver (`nvp6324_mipi_tx_regs`, bank 0x21) sets PLL/DPHY, datatype
0x1E per VC, 4-lane enable, `0x08=0x40` (frame options), `0x0F=0x01`
(MIPI_TX_FRAME_CNT_EN) — but **no explicit vertical line count / horizontal total**. That
gap is the prime suspect.

## Root-cause hypotheses (after research + PCB + vendor source)

Camera = CHIPUP **XS5018A** ISP (per PCB photo analysis, `Identify Camera Specifications.pdf`);
seller lists it **1080p**; XS5018A supports 720p/960p/1080p, actual mode is firmware-config
dependent (in the on-board MK25D40 SPI flash). Two live hypotheses:

- **(A) Wrong CSI-2 D-PHY link frequency (leading).** My driver + DT declare **621 MHz**
  (1.242 Gbps/lane); the vendor v4l2 driver (`nvp6324_mipi_driver.c`) declares
  `mipi_bps = 567 Mbps/lane` for **every** mode incl. 1080p25 → **283.5 MHz**. Telling the
  cdns D-PHY a rate ~2.19× too high miscalibrates HS-settle timing → mis-sampled line
  starts (shear) + dropped lines (720/1080) + nondeterministic lock. Camera stays 1080p.
  NB: my MIPI PLL table (bank 0x21 `0x40=0xB4`) matches *my* vendor source exactly, so
  the PLL bytes are NOT a transcription bug (the `0xBC` in radxa's fork is a different
  vendor variant).
- **(B) Camera is physically 720p** despite the listing. 720 active lines is exactly 720p;
  vendor 720p25 is carried in 720p50 timing (→ the ~50 fps). Would need the 720p decode +
  MIPI-TX path ported (vendor has both; arb_scale=0x01 for 720p).

**Test that discriminates A vs B:** keep 1080p decode, drop link freq to 283.5 MHz and
re-capture. Clean full-frame image → (A) confirmed (bake 283.5 MHz into driver default +
DT `link-frequencies` ×2, rebuild image). Still 720/shear → (B), port 720p.

Added `link_freq_hz` module param (default 621e6) to sweep the rate by reload — but a
reload corrupts the media graph (ti_csi2rx re-registers, nodes go ENOTTY), so each clean
test needs a COLD boot then `insmod /data/nvp6324.ko link_freq_hz=<hz>`. Later replaced
with a **writable V4L2_CID_LINK_FREQ menu** (`nvp6324_link_freqs[]` = 621..120 MHz ladder,
`link_freq_idx` param for default) so the rate is settable live via
`v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl link_frequency=<idx>` (cdns honors the control
value; DT still lists only 621 and does NOT reject others).

## UPDATE — link frequency is (mostly) the bug; residual intermittent lock

Confirmed **(A)**: at **283.5 MHz** (idx 7, the vendor's 567 Mbps/lane) the decoded image
is a **clear, recognizable 1080p scene** (workshop floor, cables, a blue tool) vs pure
garbage at 621 MHz. Camera is genuinely **1080p25** (30p decode = noise; re-tested at the
fixed link freq, still noise). So the D-PHY link frequency was wildly wrong (621 vs
~283.5 MHz) and that was the dominant defect.

**Residual (not yet solved):** lock is **intermittent** — frame-to-frame quality varies a
lot at a fixed rate (one dump clean, the next heavily speckled + sheared). Symptoms:
- Capture rate scales with link freq and is a multiple of 25: **621 MHz → ~50 fps (2×)**,
  **283.5 MHz → ~75 fps (3×)** (measured by `camqa --probe`). Lower link freq → HIGHER fps
  (inverse) — argues against a simple DMA-byte-clock model.
- A **zero-filled band** whose size varies per frame (~180 to ~440 lines), always at the
  bottom (frame-synced to line 0), i.e. the DMA catches the real frame at a varying
  vertical phase.
- Residual ~1.5 px/line shear.
- `bytesused` always full 4147200 (DMA completes on byte count, TI SHIM pads).
- Below 200 MHz the D-PHY drops most frames (fps collapses to ~1-2).

Caveat on method: a runtime `--set-ctrl link_frequency` switch may NOT reconfigure the
cdns D-PHY as cleanly as booting/insmod-ing at that rate (boot-at-283.5 gave a clean
frame; runtime-switch-to-283.5 gave corrupted ones — though single-frame variance could
also explain that). Treat live-sweep results as approximate; confirm a chosen rate by
insmod-ing at it on a clean boot.

**Open hypotheses for the residual:** (i) exact link freq is between menu steps / a
non-round value — try finer steps near 283.5; (ii) spurious frame-start from residual
D-PHY sync errors → 3× frames + partial captures — check cdns `--log-status` short-packet/
ECC counters; (iii) D-PHY HS-settle (bank 0x21 0x10-0x1C) needs tuning for this receiver;
(iv) a real chip MIPI frame-timing option (0x21 0x08 frame-opt / 0x0F frame-cnt). Next:
`v4l2-ctl --log-status` on the cdns subdev during a stream to read error counters.

## Error-counter sweep (the real metric) + band table

cdns band table (`drivers/phy/cadence/cdns-dphy-rx.c`, units = Mbps data-rate,
hs_clk_rate = link_freq × 2): 23 bands 80..2500 Mbps. Per-second error deltas
(CRC + unrecoverable-ECC + truncated-long, `--log-status` diff over 3 s stream), 25p:

| idx | link MHz | Mbps | err/s |
|----|----|----|----|
| 0 | 621 | 1242 | 16729 |
| 3 | 400 | 800 | 10576 |
| 6 | 300 | 600 | 10357 |
| 7 | 283.5 | 567 | 9469 |
| 8 | 260 | 520 | 6279 |
| **9** | **240** | **480** | **3701 (min)** |
| 10 | 220 | 440 | 3992 |
| 11 | 200 | 400 | 3940 |

Monotonic decrease with rate, shallow minimum ~440-480 Mbps, then capture collapses
below ~400 Mbps (D-PHY floor). **No band goes clean** (floor ~3700 err/s) → a
band-independent cause remains. "1242MHz" (idx 0) is the WORST, so the vendor comment is
not the operative rate; ~480 Mbps is the empirical best.

## ROOT CAUSE of the residual floor: cdns starts RX D-PHY *before* the TX is up

Read `cdns-csi2rx.c:csi2rx_enable_streams`: it calls `csi2rx_start()` — which
`phy_power_on` + `phy_configure`s the RX D-PHY and starts the stream FIFOs — and ONLY
THEN calls `v4l2_subdev_enable_streams(source)` (→ `nvp6324_start`, which resets +
reprograms the chip's MIPI TX). So the RX calibrates its D-PHY against a TX that is down /
mid-reset → truncated headers, CRC, nondeterministic lock, on every band.

**Fix implemented (`program_at_probe`, default on):** bring the chip's MIPI TX up at
PROBE (`nvp6324_start` at end of probe, sets `priv->hw_running`) and keep it transmitting
continuously — `enable_streams`/`disable_streams` skip start/stop when `hw_running`, so the
TX clock lane is never glitched under the RX. Default `link_freq_idx = 9` (480 Mbps).
`.ko` staged on /data (md5 be55c9ee…). Test: cold boot → `insmod /data/nvp6324.ko` →
re-run the error sweep; expect a sharp minimum near the true rate and a much lower floor.

## UPDATE 2026-09-01 — program_at_probe TESTED (cold boot). Ordering fix CONFIRMED.

Cold-booted, `insmod /data/nvp6324.ko` → dmesg "MIPI TX brought up at probe (continuous)",
clean graph (video2-7). Results vs pre-fix:

1. **Truncated-header errors: 1 → 1 (stopped accumulating).** Pre-fix these grew with the
   CRC count; they were the direct signature of RX-calibrates-before-TX-up. Gone.
2. **CRC/s at idx9 halved: 3701 → ~1871/s.**
3. **Band dependence GONE.** Re-swept idx 5–12 (660→380 Mbps) with the fix in: CRC/s is now
   **FLAT ~1800–2100 across every band** (was monotonic 16729→3701 pre-fix). fps pinned at
   ~50 across all bands (TX PLL fixed at probe; RX just captures). idx12 (380 Mbps) fps
   collapses to ~2.8 → true TX rate is >380 Mbps, RX tolerant ~400–660. **Link frequency is
   NOT the residual cause** — the pre-fix "minimum at 480" was the ordering artifact.
4. **Lock is now DETERMINISTIC, not random.** Pre-fix zero-band varied randomly 180–440
   lines/frame. Post-fix, 8 consecutive frames alternate a stable bimodal pattern.

**Residual defect now precisely characterized: ONE real 1080-line frame is split across
TWO DMA buffers.** 8-frame capture (idx9), per-frame active-line / content-tail:
- even buffers (0,2,6): ~760 active lines, content ends row **788**, shear +0.3 px/line
- odd  buffers (1,3,5,7): ~90 active lines, content ends row **293**, shear ~0
- 788 + 293 = **1081 ≈ 1080**. Stitching even[0:788] + odd[0:292] → a **coherent full 1080
  scene** (`stitch_*.png`: workshop, cables, floor). So the TI SHIM makes 2 buffers per real
  25 fps frame (→ the 50 fps) — even buffer = top 788 lines, odd = bottom ~292, each
  zero-padded to 1080. This is the "top 2/3" the user saw.

Two remaining defects, both TX raster-geometry (NOT link freq):
- **(1) Frame split at line ~788** — an extra Frame-Start (FS) mid-frame → 2 buffers/frame.
- **(2) Horizontal shear ~2 px/line** (wraps ~90% width; the vertical tear on the right).
- Plus a **band-independent ~2000 CRC/s** speckle floor.

MIPI-TX table has `{0x21,0x08,0x40}` (frame options) + `{0x21,0x0F,0x01}` (FRAME_CNT_EN) but
**no explicit lines-per-frame / pixels-per-line register** — the raster geometry gap. Next:
root-cause the mid-frame FS (frame-opt 0x08 / frame-cnt 0x0F / frame-opt nibble 0x3E, or a
missing VVL/VVH-style line-count) and the line-length mismatch (shear).

### Frame-split mechanism — DECISIVE data (2026-09-01)

**Per-buffer DQBUF timestamps** (`v4l2-ctl --stream-mmap --verbose`, ts-src-eof) alternate a
fixed **22.6 ms / 17.3 ms** (sum 39.9 ms = one 25 fps period). So the TX emits **two FE/FS per
real frame at fixed phases** — this is real deterministic TX framing, NOT corruption-induced
splitting (that would be random) and NOT continuous readout crossing a boundary (the 1.31:1
time ratio ≠ the 788/292 = 2.7:1 line ratio). The chip is **framing on fields**, not frames.

**MIPI-TX config is byte-identical to vendor** for 1080p: vendor `mipi_tx_init()`
(`jaguar1_mipi.c`) sets `0x08=0x40`, `0x0F=0x01`; the disabled 720p branch sets the SAME
`0x08/0x0F` — so frame-vs-field framing is **not** in the 0x21 bank. Vendor
`decoder_mipi_fmtdef[AHD20_1080P_25P]` = `{arb_scale=0x00, mipi_frame_opt=0x00}` — my driver
matches exactly. So the field-framing originates on the **analog decode / VSYNC** side (the
MIPI TX just follows the decoder's video VSYNC), and my per-channel video table is a faithful
port. Bank-0x05 auto-detect regs read 0x00 (auto-detect is off; I program a fixed format).

`hw_running` means the TX runs continuously and the driver won't reprogram it, so decoder
registers can be poked LIVE via `i2cset -f -y 4 0x31 0xff <bank>; i2cset -f -y 4 0x31 <reg>
<val>` and the effect seen immediately (i2c writes don't touch /dev/video2, so camqa can keep
streaming). Seconds per iteration; only cold-boot to validate the final driver change.
CAUTION: a bare mid-stream write to an arbiter/TX register can STALL streaming and a plain
revert may not recover it — the arbiter needs its full re-init cycle (below). `0x21 0x08=0x44`
also hung the stream. Wrap experiments so they always restore + verify.

## BREAKTHROUGH 2026-09-01 — the frame split is the 4-VC ARBITER (oversubscription)

The driver sets up ALL 4 channels and enables all 4 VCs in the MIPI arbiter (`arb_enable`,
bank 0x20 reg 0x00 = `en_param` = 0xFF, where vendor accumulates `en_param |= 0x11<<ch`).
But only ch0 has a camera (`0xA0=0x0E`). The 3 signal-less channels free-run and the arbiter
muxes 4 streams — advisor's bandwidth math: 4 × 1080p25 UYVY ≈ 3.4 Gbps onto a link carrying
only 4 × 480 Mbps = 1.92 Gbps (175% oversubscribed). The arbiter's scheduling boundaries (NOT
video boundaries) produce the 2-buffers-per-frame split.

**Live test (VC0-only): decisive.** Full vendor `arb_init` cycle ending in `en_param=0x11`
(ch0 only): bank0x20 `0x00=0x00; 0x40=0x01; 0x0F=0x00; 0x0D=0x01; 0x40=0x00; 0x00=0x11`.
Result: **fps = 25.00 (was 50), split GONE, frames now FULL 1080 lines** (zeroTailStart=1080,
no zero band), **stable frame-to-frame** (~858 detailed lines, meanY 63.1 across 8 frames).
The captured image is a coherent full-frame scene. So the split is 100% the 4-VC arbiter.

**Fix direction:** only set up + enable the VCs that have a camera. For the 1-camera bring-up,
enable VC0 only. For the eventual 4-camera goal the link must actually carry 4 VCs → the
vendor's **1242 Mbps** link freq is probably REQUIRED for 4-VC (4 × 1242 = 4.97 Gbps > 3.4);
the "480 Mbps empirical best" was found under the buggy pre-fix single-VC-ish conditions and
may be wrong for 4-VC. NEXT: re-sweep high bands (idx0–4) post-fix, both VC0-only and 4-VC.

**4-VC is NOT bandwidth oversubscription (theory refuted).** 4-VC (arb 0xFF) still splits
(fps ~50) at EVERY link freq incl. idx0 = 1242 Mbps (4 × 1242 = 4.97 Gbps ≫ the 3.4 Gbps a
4-camera load needs). So the split is the arbiter's behavior with signal-less VCs enabled, not
link capacity. Fix = enable only VCs that carry a camera (driver `vc_mask`, default VC0). Open
question for the 4-camera goal: whether 4 REAL synchronized cameras also split (untestable with
1 camera) — may need per-VC frame-sync align or just works when all 4 have genuine video.

VC0-only link-freq sweep (CRC/s, all fps=25.00): idx0/1242=13598, idx2/900=8746, idx4/360=6008,
idx6/300=5912, idx9/240=4865 → monotonic, lowest at idx9 (of those); shear tracks it
(idx0 +1.38 → idx4 +0.30 px/line). CRC never hits zero — residual per-frame D-PHY resync.

## ★ SOLVED (2026-09-01) — the CRC floor was the TX bit-rate (1242 Mbps eye), fix = 594 Mbps

Web research (primary sources: torvalds/linux cdns-csi2rx.c, cdns-dphy-rx.c, j721e-csi2rx.c;
rockchip-linux jaguar1_drv) nailed it: **the driver TX was running at 1242 Mbps/lane — Nextchip's
"FHD x4ch" rate — a marginal, dirty eye on this board.** One 1080p25 UYVY stream needs only
~207 Mbps/lane; 1242 is ~6x overkill and its eye is the CRC floor. All-session RX-band sweeping
couldn't fix it because the band only sets RX hs-settle; the marginal signal is the TX's.

Nextchip's `mipi_tx_init()` has FOUR selectable rates, **each with its own matched 0x10-0x1C
HS/CLK timing block** (they are NOT independent of the PLL 0x40-0x42):

| mclk | 0x40 0x41 0x42 | use |
|----|----|----|
| 1242 | B4 00 03 | FHD x4ch (stock; marginal here) |
| 756  | DC 10 03 | general |
| **594** | **CC 10 03** | **HD x4ch (fix for 1x1080p)** |
| 378  | DC 20 03 | low-clock test |

**Live test @ 594 Mbps TX (VC0-only), RX-band sweep idx 2/4/6/8/10: CRC/s = 0 at EVERY band,
25.00 fps.** Captured frame: **shear +0.05 px/line (was +0.7..+1.4), full 1080 lines, luma
broadcast-clean.** So the 594 timing block fixes CRC and shear completely.

**Residual (separate layer):** color/chroma still speckles (magenta/green/yellow blotches in
high-detail regions). With CRC=0 the UYVY bytes are intact → this is **analog chroma decode
(cross-color)**, not MIPI — and it was present at 1242 too, just masked by the transmission
noise. It is spatially content-locked (edges/bright areas), consistent with luma→chroma
cross-talk on this dim/detailed scene. A different tuning layer (comb filter / chroma gain /
saturation) or just scene/lighting; deprioritized — luma is clean.

**Baked into driver:** per-rate PLL+timing blocks `nvp6324_mipi_pll_{1242,756,594,378}` + common
`nvp6324_mipi_tail_regs`; `mipi_mclk` param (default 594) selects; `link_freq_idx` default → 6
(300 MHz DDR = 600 Mbps band, pairs with 594). `.ko` md5 f745a5a9, staged /data/nvp6324-594.ko.
**4-camera caveat:** 4x1080p25 needs ~829 Mbps/lane, so a 4-cam build needs mipi_mclk=1242
(idx0) — where the eye may again be marginal; revisit signal integrity then (or run 4ch at 720p).

## ★ SOLVED (2026-09-02) — the autoload "EPIPE" was missing format propagation, NOT a probe race

After baking the driver into the image with autoload, a cold boot autoloads it correctly
(chip: PLL 0xCC/594, arb 0x11/VC0, camera locked 0xA0=0x0E, link_freq idx6) — but
`v4l2-ctl -d /dev/video2 --stream-mmap` fails **VIDIOC_STREAMON = -EPIPE**. The identical
driver streamed clean pre-flash when insmod'd by hand.

**A probe-time race was hypothesized and then DISPROVEN by direct measurement.** On a clean
cold boot (graph intact — verified `/dev/video2` = ticsi2rx context 0, no phantom nodes):
- `dmesg`: probe at 1.83s, "MIPI TX brought up at probe" at 1.96s.
- Forced i2c read (driver holds 0x31, so `i2cget -f -y 4 0x31 ...`): `0xA0 = 0x0E`
  (ch0 present), bank 0x21 `0x40=0xCC 0x41=0x10` (the 594-Mbps PLL block). **Chip state is
  exactly correct.** Yet STREAMON EPIPE'd persistently (3/3), with the camera long since
  locked — so a corrupted-at-probe chip state is ruled out (a corrupt state would NOT stream
  after any userspace fix that doesn't touch the chip).

**Actual root cause: no userspace format propagation.** Dumping every subdev pad showed the
mismatch:
- nvp6324 source (subdev2 pad4): **1920×1080** (driver default — correct).
- cdns bridge (subdev1 pad0/pad1) and ti-csi2rx (subdev0 pad0): **640×480** (V4L2 pad default).

V4L2 link validation compares adjacent pad formats at STREAMON, so the 1920×1080↔640×480 link
is rejected with **-EPIPE**. Two `media-ctl -V` calls pushing 1920×1080 down the chain
(bridge sink `:0/0`, ti-csi2rx sink `:0/0`; the bridge propagates to its own source pad
internally) → STREAMON succeeds, **25.00 fps, CRC=0**. The pre-flash "manual insmod worked"
runs had always run that media-ctl setup by hand first; the autoloaded boot never did.
Routing was already correct at boot (all routes `ENABLED,IMMUTABLE` from driver+DT), so ONLY
the format needed setting. Format persists in each subdev's active state across
STREAMOFF/STREAMON, so a boot-time one-shot suffices (verified: fresh STREAMON after the
setup, with no re-set, still 25fps).

The `program_at_probe` ordering fix stays valid and is NOT implicated: the AHD decoder
re-locks continuously as the camera ISP comes up (that is exactly why `0xA0=0x0E` and frames
are clean now), so programming at 1.96s against a not-yet-locked input does not durably
corrupt anything. **Do NOT add a probe-time 0xA0 poll / hw_running gate — it would fix a
non-bug.**

**Fix shipped:** `recipes-ultima/nvp6324-csi-setup` — a systemd oneshot (`WantedBy
multi-user.target`, after modules-load/udev-trigger) that waits (bounded ~20s) for the media
graph to complete (the Cadence bridge async-probes at ~6.5s, well after the nvp6324 i2c probe
at ~1.8s), then applies the VC0 format with media-ctl. Lives in the board layer, not the
board-agnostic ultima-app, because the entity names are SoC/DT-specific. Deliberately NOT
`Before=ultima-app.service`: the app renders camera placeholders today and does not open
/dev/video2, so gating the boot-optimized dash on it would only add latency; revisit when the
app grows live feeds (add Before= + Wants, or have the app retry-open the node). The setup
script was validated live end-to-end: break pipeline to 640×480 → STREAMON EPIPE → run script
→ 1920×1080 → STREAMON 25fps.

**VERIFIED on the flashed image (2026-09-02, commit c8ad234), cold boot, zero manual steps:**
`nvp6324-csi-setup.service` = enabled/preset-enabled, `active (exited) status=0/SUCCESS`
(142ms), journal shows `VC0 pipeline set to UYVY 1920x1080 (/dev/video2 ready)`; the cdns/ti
subdev pads read 1920×1080 with no hand-run media-ctl; `v4l2-ctl --stream-mmap` on /dev/video2
→ 25.00 fps straight off the boot. The autoload → oneshot → STREAMON path is fully hands-free.

CAUTION LEARNED (still true): unbind/rebind the nvp6324 i2c driver CORRUPTS the media graph
(video2→ENOTTY, new video8-13) — same as a module reload. Do NOT re-probe to test.

**camqa node-mapping bug (open):** camqa defaults to `/dev/video0..3`, but on this SoC
`video0`=Wave5 VPU decoder, `video1`=VPU encoder; the CSI capture contexts are `video2..7`
(VC0=`video2`). `camqa --probe` will misreport unless pointed at the right base node. The
gauge app's camera bring-up needs the same mapping.

**Build housekeeping (non-blocking, addressed in this commit):** the falcon-fit FIT didn't
rebuild on a dtso edit — `do_compile[file-checksums]` on a bind-mounted path doesn't
invalidate sstate; `do_compile[nostamp] = "1"` on ultima-falcon-fit.bb (same trap nvp6324.bb
fixed with do_unpack[nostamp]). `KERNEL_MODULE_AUTOLOAD` emits no modules-load.d entry for
an out-of-tree module.bbclass recipe... — WRONG, corrected from hardware: KERNEL_MODULE_AUTOLOAD
DOES write `/usr/lib/modules-load.d/nvp6324.conf`, and `systemd-modules-load` inserts the
module early (~1.8s, journal `Inserted module 'nvp6324'`), before this image's deferred udev
coldplug. The DT modalias is a redundant fallback. Recipe comment corrected to match.

## CRC-floor hunt (2026-09-01) — datasheet register map + what does NOT fix it

Got the **N4 datasheet register map** (`docs/MY-CAM004M/Datasheet-N4.pdf`, §5.1.17/5.2.7,
BANK21 MIPI TX). Decoded 0x08: bit7 HRES_IN, [6:4] LP_SLEW_IN, **bit3 CONT_TX_CLK**, bit2
RESTART_EN, bit1 STANDBY_EOF, bit0 STANDBY_EN. My 0x08=0x40 → LP_SLEW=4, CONT_TX_CLK=0
(non-continuous). 0x10–0x1C = D-PHY HS/CLK timing (T_HS_ZERO/PREPARE/TRAIL/EXIT,
T_CLK_*, T_LPX, T_WAKE_UP, T_INIT, T_BGAP). 0x30/0x31 LINE_BYTE_CNT default 0x0F00=3840 =
1920×2 → **line length correct** (rules out an H-total register, confirms shear is error-driven).
0x3E/0x3F FRAME_OPT default 0 = "Free-running for Progressive" → **correct** for 1080p25.
0x45 MIPI_DATA_CLK_SEL default 0x02 = ÷2. 0x07 [3:2] LANES_ACTIVE: Others=4-lane (my 0x0F ok).

Tested LIVE, NONE reduced the CRC floor:
- **Continuous clock (0x08=0x48, bit3=1):** streams fine (fps 25, the earlier 0x44 hang was
  RESTART_EN, not this) but CRC 4236→4708 = unchanged. So the TX/RX clock-continuity mismatch
  is NOT the cause. (cdns RX uses `phy_mipi_dphy_get_default_config_for_hsclk(link_freq)` + a
  23-band table; it ignores clock-noncontinuous; DT has none → RX assumes continuous.)
- **T_HS_ZERO (0x10) sweep** 0x13→0x34: CRC 4646/4743/4564/4449 = flat (noise). HS preamble
  length is not the lever.

**Key unexplained clue:** 4-VC CRC (~1871/s) is LOWER than VC0-only (~4500/s), opposite of eye
closure. Fits "errors at frame-start re-acquisition after the long inter-frame LP idle" (25fps
single-VC idles ~12ms/frame; 4-VC keeps the link busy). But continuous-clock didn't help and a
per-row corruption scan wasn't top-concentrated (metric confounded by scene content). Leading
remaining theories: (a) data-lane LP→HS re-acquisition / eye margin (signal integrity of the
cam-board→BeagleY CSI path — largely HW-bound; only lever is lower link rate, already applied);
(b) the split-causing 4-VC config is the DESIGNED path and may run CLEAN once all 4 AHD inputs
have REAL synchronized cameras (untestable with 1 camera) — i.e. the 1-camera VC0-only CRC
penalty may not apply to the 4-camera end state. Web-research pass in progress for known fixes.

**Still residual at VC0-only:** shear ~0.85 px/line (the diagonal tear at ~80% width) and
CRC ~5257/s (oddly HIGHER than 4-VC's ~1871 — CRC/s isn't cleanly comparable across framings;
at 25fps the link idles longer in LP between frames → more LP↔HS transitions). Advisor: shear
tracks CRC; chase the CRC floor (link freq / D-PHY HS timing 0x21 0x10–0x1C), not a separate
H-total register. Note (advisor): `ti_csi2rx` hardcodes `vb2_set_plane_payload(...,sizeimage)`
so `bytesused=4147200` carries no info; zero rows are unwritten vb2 memory, not SHIM padding.

## Fast out-of-tree .ko cross-build (no bitbake)

```
docker run --rm -v falcon-yocto-build:/home/builder/yocto -v <camdriver>:/src:ro -v <out>:/out \
  falcon-yocto:latest bash -c '
    G=/home/builder/yocto/tisdk/build-beagley-ai/arago-tmp-default-glibc
    export PATH=$G/sysroots-components/aarch64/gcc-cross-aarch64/usr/bin/aarch64-oe-linux:\
$G/sysroots-components/aarch64/binutils-cross-aarch64/usr/bin/aarch64-oe-linux:$PATH
    mkdir /tmp/b && cp /src/nvp6324.c /src/nvp6324_regs.h /src/Makefile /tmp/b/
    make -C $G/work-shared/beagley-ai/kernel-build-artifacts M=/tmp/b \
      ARCH=arm64 CROSS_COMPILE=aarch64-oe-linux- modules && cp /tmp/b/nvp6324.ko /out/'
```
(Volume MUST mount at `/home/builder/yocto` — the kernel build dir has absolute paths.)
camqa cross-build uses kmsxx's recipe-sysroot(+native); see qa/README.md.
