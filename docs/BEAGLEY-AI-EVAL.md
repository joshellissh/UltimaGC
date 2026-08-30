# BeagleY-AI (TI AM67A / J722S) evaluation — for hardware dashcam recording

**Status: paper evaluation only (2026-08-27). No BeagleY-AI hardware acquired,
nothing built or measured on-target. This is a go/no-go research note to decide
whether a board switch is worth pursuing for the 4-camera recording feature.**

Every spec below is either **[TI-verified]** (quoted from TI's datasheet / SDK
docs / U-Boot source — see Sources) or **[inferred]** (architectural reasoning
from the verified facts). Treat `[inferred]` claims as things to confirm on
hardware, not as settled. This project has already been burned once trusting a
spec that turned out wrong (see `verify-hardware-specs-not-seller-listings` in
memory, and note that this very evaluation reversed an initial wrong guess that
AM67A "has no hardware encoder" — it does).

---

## Why this evaluation exists: the recording bottleneck on the current board

The current board is BeaglePlay (TI **AM625**, 4×A53 + PowerVR AXE-1-16M GPU).
The dashcam-recording spike (see the `dashcam-recording` branch, and NOTES.md's
mention of it being held out of the CAN-adapter build) established that recording
the 4 cameras to disk is **bottlenecked on software video encode**, because:

- **The AM625 has no hardware video encoder (no VPU).** Encode must run on the
  A53 cores.
- **Measured on hardware:** software JPEG via turbojpeg ≈ **67 ms/frame @ 1080p**
  (≈ 15 fps for one stream on one core). Software H.264 via OpenH264 ≈
  **233 ms/frame** (≈ 3.5 fps vs a 15 fps target) — tried and reverted, not
  viable.
- **GPU offload is a dead end here:** the AXE-1-16M is a small graphics GPU, not
  a codec; a shader JPEG encoder leaves entropy coding + a stalling readback on
  the CPU anyway, and the GPU is already the display's critical path (a visible
  mirror overlay alone drops the window to ~16 fps — GPU-bound).
- **Realistic ceiling on AM625:** ~4 cameras @ **~10 fps MJPEG** (or drop to the
  driver's 720p mode to roughly halve per-frame cost), and every recorded fps is
  taken directly from the display the driver is looking at.
- **Storage is a separate blocker regardless:** rootfs is read-only, `/data` is
  ~14 MB. There is no on-board destination sized for continuous multi-cam video.

The only thing that removes the encode bottleneck is a **hardware video
encoder** — which the AM625 does not have. That is what motivates looking at the
AM67A.

---

## BeagleY-AI = TI AM67A (J722S) — core specs [TI-verified]

| Block | AM67A (BeagleY-AI) | AM625 (BeaglePlay, current) |
|---|---|---|
| CPU | 4× Cortex-A53 @ 1.4 GHz | 4× Cortex-A53 @ 1.4 GHz |
| GPU | IMG **BXS-4-64**, ~50 GFLOPS | PowerVR AXE-1-16M (much smaller) |
| DSP / AI | 2× C7x (40 GFLOPS ea) + 2× MMA (2 TOPS ea) = 4 TOPS | none |
| **Video codec** | **Wave5 HW encode+decode, H.264 + H.265 + MJPEG** | **none (no VPU)** |
| Family bootflow | "am62xxx extended SoC family" (same as AM62x) | AM62x |

The AM67A is essentially "the AM625's A53 complex + a bigger GPU + DL
accelerators + a hardware video codec." Same CPU tier — so *software* encode
would be no faster; the win is entirely the Wave5 codec.

---

## Does the recording problem go away? Yes — encode is solved [TI-verified]

The Wave5 codec IP on AM67A:

- **Hardware H.264 and H.265 encode + decode**, plus **hardware Motion-JPEG
  encode** (so even the exact MJPEG path from the spike could be offloaded).
- Exposed as standard **V4L2 M2M** encoders (`v4l2h264enc` / `v4l2h265enc`) with
  GStreamer plugins — a well-trodden integration path, **not** custom shader work.
- Ceiling: **up to 4K60 / ~500 MP/s** encode. Driver supports **8× 1080p encode
  channels** with the default CMA configuration.

**Fit for 4 cameras** (the AHD cameras are 1080p**25**, PAL):

- 4 × 1080p25 = 4 × 51.8 = **~207 MP/s ≈ 41 % of the 500 MP/s ceiling** (≈ 50 %
  even at 30 fps). Half the driver's 8-channel limit.
- With zero-copy dma-buf from capture → V4L2 encoder, the A53s never touch a
  pixel. **CPU load for recording drops from "impossible" to near-idle.** `[inferred]`
- H.265 also shrinks the storage side: 4 × 1080p H.265 @ ~4 Mbit/s ≈ **~2 MB/s ≈
  ~7 GB/hr**, vs raw MJPEG's ~12 MB/s. Storage *bandwidth* becomes a non-issue;
  storage *destination* (a card / partition) is still needed. `[inferred]`

**Conclusion: on the AM67A, 4×1080p25 hardware recording is comfortably within
budget, at ~zero CPU cost.** The encoder is not the tight resource.

### Why NOT the BeagleBone AI-64 (TDA4VM / J721E) [TI-verified]
Considered as the alternative "AI beagle." Its encoder (Imagination VXE384) is
**H.264 only, max 1080p, and no more than 2 channels in parallel** — cannot
hardware-encode 4× 1080p simultaneously. **For this specific job the AM67A beats
the TDA4VM.** (An earlier steer in this eval toward the AI-64 was wrong on the
specifics.)

---

## The real remaining risk: 4-camera simultaneous capture

Switching boards moves the risk *off encode and onto capture* — which is already
the item NOTES.md flags as **"still open: simultaneous multi-camera capture"** on
the current board. That question does not disappear on the AM67A; it must be
re-answered on the new SoC.

- **The path:** 4 analog AHD cameras → the **N4 (nvp6324)** decoder, which muxes
  all 4 onto **one CSI-2 link as 4 virtual channels** → the SoC's CSI-2 receiver
  demuxes them to 4 V4L2 nodes. The pivotal unknown is whether the J722S CSI-2 RX
  driver does that **4-VC demux to 4 independent 1080p25 streams simultaneously**,
  without dropped frames or cross-talk. `[inferred — needs verification]`
- The AM67A is a surround-view-class capture subsystem (more CSI + a proper ISP
  than the AM625), so this is the SoC it is *supposed* to work on — but the
  `mycam004m` / N4 driver + CSI pipeline would need porting/revalidating on
  J722S. `[inferred]`

### Validation plan (answers "will it work" *before* any full port)
Do all of this on a **stock BeagleY-AI running TI's Processor SDK Linux** — not
the Falcon/Yocto stack. Prove the SoC capability in isolation first (same pattern
this project used to prove the GPU independent of Qt).

- **Phase 0 — desk / paper (hours, no hardware):**
  1. Confirm the J722S CSI-2 RX driver (`j721e-csi2rx` / Cadence `cdns-csi2rx`)
     supports **multi-VC / multi-stream demux to separate nodes** in the SDK's
     kernel version (this has been an evolving upstream feature — kernel version
     matters).
  2. Confirm DPHY lane budget on BeagleY-AI's camera connector vs what the N4 needs.
  3. Confirm CMA budget: the "8ch 1080p encode" figure is *encode alone*; 4
     capture pipelines + encoder buffers share CMA.
  4. Check whether TI ships an nvp6324/AHD or surround-view/DVR reference on J722S
     — if so, that's most of the answer for free.
- **Phase 1 — cheap hardware smoke test (~$70 board, days):**
  5. Bring up ONE camera through the real path (N4 driver, one AHD cam locked,
     one V4L2 node at 1080p25).
  6. **Pivotal test:** 4 cameras simultaneously — 4 V4L2 nodes at 1080p25 at once
     (`media-ctl -p` topology, then `v4l2-ctl`); verify sustained fps, no drops,
     no VC cross-talk.
- **Phase 2 — encode + storage (days):**
  7. `gst-launch`: 4× `v4l2src ! v4l2h265enc ! splitmuxsink`; confirm 4 sustained
     1080p25 encodes, measure CPU (should be near-idle) and CMA headroom, confirm
     segmented rotation to storage.

Phases 0–2 answer the go/no-go **without touching Falcon boot, the Ultima app, or
CAN.** The full port happens only if they pass.

**Effort shape:** Phase 0 = hours. Phase 1 = the real work (porting the N4 driver
to the J722S capture stack + proving 4-VC capture) — days to ~2 weeks depending
on how much the CSI2RX driver already does. Phase 2 = days, standard GStreamer.

### CSI connector compatibility — same 22-pin RPi standard, but not plug-and-play

Good news at the connector level, verified from BeagleBoard docs + this project's
own `docs/MY-CAM to BeaglePlay Pin Mapping.pdf`:

- **BeaglePlay's camera connector (J17) is a 22-pin 0.5 mm CSI-2 connector with the
  standard Raspberry Pi 22-pin pinout** — 4 data lanes (D0–D3 ±), clock (CK ±),
  I²C (SCL/SDA on 20/21), IO0/IO1 (17/18), 3.3 V (22). `[verified — project pin map]`
- **BeagleY-AI uses the same 22-pin RPi-5-style connectors** (two of them; CSI0 is
  camera-only, CSI1 is muxed with DSI). `[TI/BeagleBoard-verified]`
- So the **CSI-2 signal core lines up 1:1** — a stock RPi camera FFC would seat and
  work on either board. No exotic camera-interface adapter needed; this slightly
  *de-risks* the switch.

**But this project does not use a stock FFC**, so "pin compatible" ≠ "plug and
play":

- The cameras attach via the **MY-CAM004M (N4/nvp6324 AHD decoder) board**, whose
  output is a **24-pin** header (adds `MCLK`, `PWRDN`, `RST_N`, `PWREN`, and a
  **`VDD_5V`** pin). The BeaglePlay hookup is a **custom harness**: the 4 lanes +
  clock + I²C map 1:1 to J17, but several control pins are struck through/unused,
  and the **`VDD_5V` + `PWREN` leads run *outside* the CSI ribbon** (the 22-pin
  connector only offers 3.3 V; the N4 needs 5 V). That off-ribbon 5 V/control
  wiring is the board-specific part to re-establish on BeagleY-AI. `[verified — project pin map]`
- **Regardless of the connector**, the CSI-2 RX instance, the I²C controller/bus,
  and the reset/enable GPIOs land on different SoC pins + DT nodes on the AM67A —
  so the device-tree/driver mapping is redone either way (this is the Phase-1
  camera-port work above, not new scope). `[inferred]`

**Connector verdict:** the physical CSI connector is *not* a blocker and needs no
adapter (same 22-pin RPi standard); what does not transfer for free is the N4
board's off-ribbon 5 V/control harness and the SoC-side DT/driver plumbing.

---

## Boot time: can it match the current ~4.5 s to first Qt frame?

**Current AM625 baseline [verified on-hardware, see NOTES.md]:** power-on → first
Qt frame ≈ **4.5 s on eMMC** (kernel-clock to first frame ≈ 3.3 s), down from a
~8.9 s baseline. The win is *mostly* TI **Falcon mode** — R5 SPL loading the
kernel directly, skipping A53-SPL → U-Boot → GRUB (that skip saved ~5.3 s of real
boot + a ~4.5 s GRUB timeout).

Splitting the question by boot half:

- **Kernel → first frame (~3.3 s):** userspace — systemd ordering, the
  `ultima-app.service` decoupling, read-only rootfs, Qt/EGL init. App/OS-level,
  not SoC-specific → **should carry over roughly intact**, maybe slightly heavier
  for the bigger GPU + more firmware to init. `[inferred]`
- **Power-on → kernel:** this is where a board switch could lose ground, and it
  turns on **Falcon availability for J722S.**

### Falcon on J722S — verified status
- **Mechanism: family-capable.** U-Boot's J722S board doc states its **"bootflow
  is exactly the same as all SoCs in the am62xxx extended SoC family."** Falcon is
  an **R5-SPL-stage K3 feature** (`CONFIG_SPL_OS_BOOT` + `k3_r5_falcon.config` +
  `tispl_falcon.bin`) — the same A53-SPL/U-Boot skip these am62x-family parts
  support. So J722S's A53 critical-path bootflow is the **same shape** the AM625's
  Falcon already optimizes. `[TI-verified]`
  - *(This corrects an earlier worry in this eval that J722S is heavyweight
    Jacinto boot where Falcon might not exist. The extra cores — C7x/MMA — exist
    but are not on the A53 boot path Falcon skips.)*
- **Productization: NOT wired for J722S out of the box.** `[TI-verified]`
  - U-Boot's J722S board doc **does not document Falcon** (no `SPL_OS_BOOT` /
    `tispl_falcon.bin` / `k3_r5_falcon` mention).
  - The meta-ti `ti-falcon` Yocto override covers **only the AM62 EVMs**
    (`am62xx`, `am62axx`, `am62pxx`, `am62xx-lp`). **J722S / AM67A are not in the
    list.**

### What that means

**Update (2026-08-29, hardware in hand): this turned out to be a dead end, not
bounded work — corrected below, see `beagleplay-falcon/NOTES.md` "Falcon on
J722S/BeagleY-AI: dead end" for the full trail.** The C-code/binman gap
itself *was* portable (bb.org's fork lacks the falcon SPL logic and binman
template TI staging has, but every API/firmware-reference it would need
already exists in bb.org's tree). The actual blocker is one level lower: **TI
has never published the TIFS stub firmware (`ti-fs-stub-firmware-j722s-*`)
the falcon fast-path's security handshake depends on** — confirmed absent on
every branch of TI's `ti-linux-firmware` repo, rechecked live against a fresh
fetch (a branch updated 12 days prior still had nothing for J722S). It exists
only for the four machines TI's own `ti-falcon` override already lists
(am62xx/am62axx/am62pxx/am62xx-lp). This is a TI-hasn't-shipped-it problem,
not a missing-wiring problem — no amount of Yocto/patch work here fixes it.
The "family-capable" framing below was based on the SPL bootflow being
shared across the am62xxx-extended family (true, per U-Boot's own J722S
board doc) — that does **not** imply TI has released the same firmware for
every SoC in that family, which is the part that actually matters.

**Original (now-superseded) framing, kept for context:** You would extend
the `ti-falcon` wiring to J722S yourself, following the `am62pxx` /
`am62axx` pattern (machine override, R5 falcon defconfig fragment,
`tispl_falcon.bin` packaging). Precedent exists: this project already closed
the same kind of gap on AM625 (NOTES.md: meta-ti "ships Falcon Mode support,
but it's wired [specifically]… Missing package/config wiring"). This was
believed to be bounded config/recipe work — it was not; see the update above.

**Boot verdict:** the ~4.5 s-class first-frame target is **not reachable via
Falcon on this board** until TI publishes J722S stub firmware. The remaining
lever on this SoC is OS-level: suppressing UART console spam (`quiet`) and
trimming the GRUB timeout, the same non-Falcon-specific wins BeaglePlay's own
numbers partially came from.

---

## Storage: no onboard eMMC — SD stays in the boot path

**BeagleY-AI has no eMMC at all** — the only integrated storage is the microSD
slot (`MMC1` on AM67A). This is a real difference from the current BeaglePlay
setup, which boots from eMMC by default (NOTES.md: eMMC + Falcon together get
power-on → first Qt frame to ~4.5 s). `[BeagleBoard-verified]`

- **Boot ROM order: SD card → Ethernet netboot** if no SD is present. No
  USB-boot or direct-NVMe-boot at the ROM stage. `[BeagleBoard-verified]`
- **microSD does support UHS-I** (1.8 V voltage-switching circuit present), so a
  good UHS-I U3/A2 card is the ceiling if staying SD-only — random 4K read/write
  matters more for boot time than sequential MB/s, and varies a lot card-to-card
  even within the same speed class. `[BeagleBoard-verified]`
- **PCIe Gen2 x1 → NVMe is the real higher-speed option**, but it doesn't
  eliminate the SD card: the AM67A boot ROM can't boot straight to NVMe, so SD
  still has to hold the initial SPL/U-Boot stage, which then chain-loads the
  rootfs from NVMe. What this buys you is moving the **heavy stuff** (rootfs,
  Qt app, assets) off SD onto NVMe, while SD's job shrinks to a small boot
  partition. Connector is a Raspberry Pi 5-style flat-flex HAT connector (not a
  standard M.2 slot) wired to SERDES1, so an RPi5-style M.2 HAT+ (or a
  BeagleY-specific equivalent, if one exists) is needed to actually plug in a
  drive. `[BeagleBoard-verified]`
- **USB 3.1 (4 ports via hub)** is available as a fallback storage path but has
  the same U-Boot-mediated chain-load caveat as NVMe, and is slower besides —
  only worth it to avoid a HAT. `[BeagleBoard-verified]`

**Storage verdict:** you can't get away from needing an SD card for the initial
boot stage given current ROM constraints, so **the SD dependency this eval's
opening question was about doesn't go away**. PCIe NVMe is the closest
equivalent to what eMMC would've bought — it's the option to reach for if
rootfs/asset read speed (not the tiny SPL-load step) turns out to be the actual
bottleneck once Falcon-on-J722S is measured. `[inferred]`

---

## CAN bus: no mikroBUS, but the software stack transfers

**Current setup (BeaglePlay):** MikroE CAN SPI 3.3 V click (MCP2515 controller +
SN65HVD230 transceiver) on BeaglePlay's mikroBUS socket, wired to the Syvecs S7+
CAN2 bus. Software: mainline `mcp251x` SocketCAN driver (built-in), a compile-time
DT patch declaring the MCP2515 on `main_spi2` CS0 with a 10 MHz `fixed-clock` +
INT on `main_gpio1` line 9, `70-can.rules` auto-bringing `can0` up at **1 Mbit
classical CAN**, and `CanBus` retrying `socket()`/`bind()` until the iface appears.

**BeagleY-AI has no mikroBUS** — it's RPi form-factor with a 40-pin GPIO header,
so the existing click can't seat. `[BeagleBoard-verified]`

### Two paths
- **Native AM67A MCAN** (up to 4× CAN-FD controllers) is electrically the "right"
  way, BUT per the BeagleY-AI forum the **40-pin header does not break out MCAN** —
  the signals are routed to the **CSI connectors** (multiplexed), needing a custom
  CSI-connector breakout + external transceiver + fiddly 0.5 mm soldering; community
  boards were still in development as of early 2026. Also competes with the cameras
  for a CSI connector. Not turnkey today. `[community-sourced — confirm in schematic]`
- **RPi 40-pin SPI CAN HAT (MCP2515) — recommended.** Same chipset as the current
  click, so the `mcp251x` driver, `70-can.rules`, and `CanBus` code carry over
  **unchanged**; only the DT node is new. Leaves both CSI connectors free for
  cameras. Classical CAN @ 1 Mbit is all the ECU needs, so no reason to move to
  CAN-FD. `[inferred — standard RPi SPI CAN path]`

### Recommended part + the gotcha
**Waveshare 2-Channel Isolated CAN HAT** (Amazon ASIN B087RJ6XGG): MCP2515 +
SN65HVD230, SPI, RPi 40-pin, **isolated** (an upgrade over the non-isolated click
— good for automotive), classical CAN. 2 channels (only 1 needed; 2nd is spare).

- **⚠️ Crystal frequency — the real trap.** This HAT ships with an **8 MHz (older)
  or 12 MHz (newer)** crystal, **NOT** the **10 MHz** the current DT hardcodes
  (`clock-frequency = <10000000>`). Wrong value → `can0` comes up but the bit-timing
  is off and it won't communicate. **Read the actual crystal off the board** (silver
  oval marked `12.000` / `8.000` — don't trust the listing; it comes in both
  variants) and set `fixed-clock`'s `clock-frequency` to match, plus drop
  `spi-max-frequency` (Waveshare uses 2 MHz @ 12 MHz, 1 MHz @ 8 MHz). This is a
  live instance of the `verify-hardware-specs-not-seller-listings` rule. `[verified]`
- **DT node.** Write the MCP2515 node against the **AM67A's** SPI instance + a GPIO
  for INT (same shape as the BeaglePlay patch, different pin names), and confirm
  BeagleY-AI routes SPI + the INT line to the 40-pin pins the HAT uses. `[inferred]`

**CAN verdict:** lowest-risk of the port items — a HAT swap + a DT node you've
written before, with the crystal-frequency change being the one must-not-miss edit.
(A mechanical note: an RPi HAT is a full 40-pin board, vs the compact mikroBUS click
— an enclosure consideration.)

---

## Cost of the switch (only if validation passes)

A board switch is **not a recording change — it's a platform redo.** Everything
hardware-verified today is AM625-specific:

- **TI Falcon boot** — re-wire + re-validate for J722S (bounded, precedent exists).
- **`mycam004m` / N4 camera driver** — port to the J722S CSI/capture stack
  (different CSI2RX backend; the hard-won AHD/UYVY/fps-lock knowledge carries, the
  plumbing does not). This is the biggest single item.
- **CAN** — no mikroBUS on BeagleY-AI; swap to an RPi 40-pin MCP2515 SPI CAN HAT
  (same chipset → software unchanged, only a new DT node + crystal-freq change).
  See the "CAN bus" section above. Lowest-risk item.
- **Enclosure CAD / pin mapping** — different board outline + connector layout.

Mitigant: AM67A uses the **same TI Processor SDK Linux family** (`tisdk`) this
project already builds against, and shares the am62x bootflow — so the build
system and much of the boot/rootfs knowledge transfers.

---

## Bottom line

- **Encode bottleneck:** **solved** on AM67A (Wave5 HW H.264/H.265/MJPEG, 8×1080p
  channels, standard V4L2 — 4×1080p25 is ~41 % of the ceiling at ~zero CPU). `[TI-verified]`
- **AM67A > TDA4VM** for a 4-camera job (TDA4VM's encoder caps at 2× 1080p, H.264
  only). `[TI-verified]`
- **Boot time:** ~4.5 s-class first frame is **reachable** (J722S shares the
  am62x Falcon-capable bootflow) but requires **self-provisioned Falcon wiring**
  (not productized for J722S) + validation. `[TI-verified status; timing inferred]`
- **CAN:** **lowest-risk** — no mikroBUS, but an RPi MCP2515 SPI CAN HAT (e.g.
  Waveshare 2-CH Isolated) keeps the entire CAN software stack; only a new DT node
  + a crystal-frequency change (HAT is 8/12 MHz, current DT hardcodes 10 MHz). `[verified]`
- **CSI connector:** same 22-pin RPi standard, **not a blocker / no adapter** — but
  the N4 board's off-ribbon 5 V harness + DT/driver mapping are redone. `[verified]`
- **The one real technical unknown to de-risk first:** **4-camera simultaneous
  capture** (CSI-2 VC demux on J722S) — answerable on a ~$70 board via Phases 0–2
  before committing to any port.
- **Not free:** the switch is a multi-item platform port (camera driver being the
  largest), justified only if you specifically need high-rate / high-quality
  4-cam recording that the AM625's software-encode ceiling (~4×10 fps MJPEG)
  can't deliver.

**Recommended next step:** run **Phase 0** (desk verification of J722S CSI-2 VC
demux + whether TI ships an AHD/surround-view reference) alongside the Falcon-
wiring scoping — both are "grep TI's BSP for what J722S actually supports," no
hardware required, and together they decide the switch.

---

## Sources
- AM67A datasheet / product page — <https://www.ti.com/product/AM67A>
- Processor SDK Linux for AM67A — Multimedia Video Codec (Wave5) —
  <https://software-dl.ti.com/jacinto7/esd/processor-sdk-linux-am67a/10_01_00/exports/docs/linux/Foundational_Components_Multimedia_wave5.html>
- TDA4VM datasheet — <https://www.ti.com/lit/ds/symlink/tda4vm.pdf>
- Processor SDK Linux for J721e — Multimedia Video Codec (VXE384 encoder) —
  <https://software-dl.ti.com/jacinto7/esd/processor-sdk-linux-jacinto7/08_04_00_11/exports/docs/linux/Foundational_Components_Multimedia_D5520_VXE384.html>
- U-Boot — J722S EVM board doc (shares am62x-family bootflow) —
  <https://raw.githubusercontent.com/u-boot/u-boot/master/doc/board/ti/j722s_evm.rst>
- meta-ti — "add support for falcon mode builds" patch (AM62 EVMs only) —
  <https://patchwork.yoctoproject.org/project/ti/cover/20250417113614.1780603-1-anshuld@ti.com/>
- TI Processor SDK — U-Boot Falcon Mode (AM62x) —
  <https://software-dl.ti.com/processor-sdk-linux/esd/AM62X/11_01_05_03/exports/docs/linux/Foundational_Components/U-Boot/UG-Falcon-Mode.html>
- BeagleY-AI — Design & specifications (2× 22-pin RPi-5-style CSI) —
  <https://docs.beagleboard.org/boards/beagley/ai/03-design.html>
- BeaglePlay — Design & specifications (one 4-lane CSI) —
  <https://docs.beagleboard.org/boards/beagleplay/03-design.html>
- Project — `docs/MY-CAM to BeaglePlay Pin Mapping.pdf` (BeaglePlay J17 = 22-pin
  RPi CSI-2 pinout; MY-CAM004M N4 board 24-pin custom harness)
- BeagleY-AI CAN support — BeagleBoard forum (MCAN on CSI, not the 40-pin) —
  <https://forum.beagleboard.org/t/beagley-ai-controller-area-network-can-support/40549>
- Waveshare 2-CH CAN HAT (MCP2515 + SN65HVD230, 8/12 MHz crystal variants) —
  <https://www.waveshare.com/wiki/2-CH_CAN_HAT> ; Amazon ASIN B087RJ6XGG
- BeagleY-AI — Design & specifications (storage: no eMMC, microSD UHS-I,
  PCIe Gen2 x1, USB 3.1) —
  <https://docs.beagleboard.org/boards/beagley/ai/03-design.html>
- BeagleY-AI — Booting from NVMe Drives (SD-mediated chain-load, no
  direct-ROM NVMe boot) —
  <https://docs.beagleboard.org/boards/beagley/ai/demos/expansion-nvme.html>
- BeagleY-AI Application Note — Processor SDK AM67A (boot ROM order: SD →
  Ethernet) —
  <https://software-dl.ti.com/processor-sdk-android/esd/AM67AX/11_00_01/docs/devices/AM67A/android/Application_Notes_BeagleY_AI.html>
- U-Boot — AM67A BeagleY-AI board doc —
  <https://docs.u-boot.org/en/latest/board/beagle/am67a_beagley_ai.html>
