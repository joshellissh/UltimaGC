# camdriver — MY-CAM004M (NVP6324) 4-camera driver + QA

Linux V4L2 driver bringing 4 AHD camera feeds from the **MY-CAM004M** board
(Nextchip **NVP6324 / "N4"** AHD→MIPI-CSI2 decoder) into the gauge-cluster app,
via **CSI0** on the BeagleY-AI (TI AM67A / J722S). Target: **1080p25, UYVY**, four
simultaneous feeds.

**Read `PLAN.md` first** — it has the full architecture, the hardware facts, the
milestone plan, and the open items to confirm on real hardware.

## Layout

| Path | What |
|------|------|
| `nvp6324.c` | The V4L2 sub-device driver (I2C). Multiplexed-streams source: 4 virtual channels → 4 `/dev/videoN` via the J722S CSI2RX stack. Modeled on in-tree `ds90ub960`. |
| `nvp6324_regs.h` | NVP6324 register map (distilled from the N4 datasheet + the vendor chip-programming reference). |
| `Makefile` | Out-of-tree kernel-module build (OE `module.bbclass` interface + manual `KDIR=` build). |
| `dts/k3-am67a-beagley-ai-nvp6324.dtso` | Device-tree overlay: NVP6324 on `main_i2c2` @ `0x31`, 4-lane CSI-2 into `cdns_csi2rx0`, enables `dphy0` / `cdns_csi2rx0` / `ti_csi2rx0`. |
| `qa/` | Standalone plain-C fullscreen 2×2 camera viewer (V4L2 + DRM/KMS), pushed over SSH for manual QA. See `qa/README.md`. |

The Yocto integration lives in the board layer (not here), mirroring how
`ultima-app/` is built by its own recipe:

- `beagley-ai/meta-ultima-beagley-ai-src/recipes-kernel/nvp6324/nvp6324.bb` —
  builds this folder as an out-of-tree module (bind-mounts `camdriver/` read-only,
  copies into WORKDIR).
- `.../recipes-kernel/linux/linux-bb.org/nvp6324.cfg` (+ the `linux-bb.org_%.bbappend`
  wiring) — pins the in-tree CSI2RX / D-PHY / V4L2 config the module binds onto.
- `.../recipes-core/images/tisdk-base-image.bbappend` — installs
  `kernel-module-nvp6324` + `v4l-utils` into the image.
- `beagley-ai/build.sh` — bind-mounts `camdriver/` into the build container.

## Status (M0 — board-free, done)

- Driver **cross-compiles and MODPOSTs clean** against the board's `linux-bb.org`
  6.12 kernel (all V4L2 streams / regmap / media symbols resolve).
- DT overlay **compiles** to a valid `.dtbo` with `dtc -@`.
- QA app **cross-compiles** clean against the target sysroot (aarch64 ELF, `-ldrm`).
- The CSI2RX capture stack is already `=m/=y` in the kernel config; `ti_csi2rx0`
  ships **6 DMA contexts**, so 4 virtual channels → 4 `/dev/videoN` needs no kernel
  patching (the whole reason this is tractable — see PLAN.md §1).

## Build / test

**Kernel module (via Yocto — the real path):**
```
beagley-ai/build.sh nvp6324        # builds just the module
beagley-ai/build.sh                # or the full image (installs the module)
```

**Kernel module (manual, against a prepared kernel tree):**
```
make KDIR=/path/to/kernel/build
```

**DT overlay (bench iteration):** compile with `dtc -@ -I dts -O dtb`, apply to the
deployed DTB with `fdtoverlay`, push over SSH, reboot (same discipline as the USB1
fix in `beagley-ai/NOTES.md`). Falcon boot has no runtime overlay stage, so for the
baked image the overlay is applied to the DTB at FIT-assembly time (see PLAN.md §5
— that `ultima-falcon-fit` wiring is the next integration step, deliberately left
for the M1 flash cycle).

**QA app:** see `qa/README.md` (cross-compile against the SDK sysroot, `scp` to the
board, stop `ultima-app.service` first).

## Next (needs the board — M1+)

`i2cdetect` at `0x31` + chip-ID `0xF4==0xB0`; single-VC capture (proves D-PHY lock /
link-freq / format); 4-node simultaneous capture; QA app; then the ultima-app
`CameraFeed` rebuild. See PLAN.md §10 for the staged milestones and §12 for the
hardware-confirm list.
