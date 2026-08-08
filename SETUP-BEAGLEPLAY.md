# Ultima BeaglePlay — Setup & Reproduction Guide

A guide for bringing up the Ultima gauge cluster on BeaglePlay (TI AM625). This
port shares `br2-external/package/ultima-app/` with the RPi5 build (see
`SETUP-RPI5.md`) but has its own `br2-external/board/ultima-beagleplay/` and
`br2-external/configs/ultima_beagleplay_defconfig`, and almost nothing about
the bootloader, kernel, or display stack transfers as-is.

**Status: booted and rendering on real hardware (2026-08-07).** The Qt app
runs and the gauge cluster displays over HDMI; WiFi and SSH both work. One
real hardware bug was found and fixed on first boot — see the
`S00remountro`/`S11app` entry in
[Init System & Boot Optimization](#init-system--boot-optimization) — plus a
stale SSH key in `authorized_keys` that predated this Mac. Remaining
`# TODO verify on hardware` comments in the files themselves are the
authoritative list of what's still unconfirmed (touch input, eMMC boot,
software rendering frame rate — see
[Open Questions](#open-questions--next-steps-on-real-hardware)).

Getting the build green required correcting the initial scaffolding in two
ways, both reflected in the current `ultima_beagleplay_defconfig`:
- **Toolchain**: the original config picked Bootlin's external toolchain,
  but Bootlin only publishes prebuilt toolchains for x86_64 build hosts
  (`depends on BR2_HOSTARCH = "x86_64"` in Buildroot's own
  `toolchain-external-bootlin/Config.in`) — invisible on an arm64 build VM
  (e.g. Apple Silicon via OrbStack), which silently fell back to an empty
  toolchain. Switched to Buildroot's internal toolchain
  (`BR2_TOOLCHAIN_BUILDROOT_GLIBC`), the same approach RPi5 already uses —
  works on any host architecture.
- **Bootloader-chain version pins**: the original config claimed to copy
  its TF-A/R5-loader/U-Boot/kernel versions "verbatim from upstream
  Buildroot's `beagleplay_defconfig`," but was never actually diffed
  against that file — it invented newer-looking pins (kernel 6.18.16, TF-A
  "latest LTS", U-Boot/R5-loader "2026.01") that don't match what this
  Buildroot release ships or was tested against. The U-Boot 2026.01 guess in
  particular doesn't correspond to a complete, working release: its
  `COPYING` is a symlink to `Licenses/gpl-2.0.txt`, but that file is
  missing from the tarball, which broke Buildroot's own legacy license-copy
  hook on first extract. This checkout's own `configs/beagleplay_defconfig`
  — a real, proven reference sitting right in the cloned Buildroot tree —
  pins kernel 6.10, TF-A v2.11, and U-Boot/R5-loader 2024.07; the defconfig
  now matches it exactly for these values.

---

## Table of Contents

1. [Overview](#overview)
2. [Hardware Requirements](#hardware-requirements)
3. [What's Different From RPi5](#whats-different-from-rpi5)
4. [Host Machine Setup (macOS)](#host-machine-setup-macos)
5. [Build VM Setup (Ubuntu)](#build-vm-setup-ubuntu)
6. [Project Structure](#project-structure)
7. [Buildroot Configuration](#buildroot-configuration)
8. [Kernel Configuration](#kernel-configuration)
9. [Boot Configuration](#boot-configuration)
10. [Init System & Boot Optimization](#init-system--boot-optimization)
11. [WiFi & Networking](#wifi--networking)
12. [Qt5 Application & Rendering](#qt5-application--rendering)
13. [CAN Bus Integration](#can-bus-integration)
14. [Display Configuration](#display-configuration)
15. [Build Process](#build-process)
16. [Flashing](#flashing)
17. [Debugging](#debugging)
18. [Open Questions / Next Steps on Real Hardware](#open-questions--next-steps-on-real-hardware)

---

## Overview

**What this is (intended)**: The same fullscreen Qt5 QML gauge cluster as the
RPi5 build, running on a BeaglePlay over HDMI, fed by the same ODrive USB-CAN
adapter reading the Syvecs S7+'s CAN2 bus.

**Key design decisions, and how they differ from RPi5**:
- BusyBox init (not systemd) for fast boot — same as RPi5.
- Read-only root filesystem + separate writable `/data` partition for
  odometer state — same pattern as RPi5.
- **Rendering is software, not GPU-accelerated.** RPi5 uses Qt5's `eglfs`
  QPA plugin over Mesa's open-source V3D driver. BeaglePlay's GPU
  (Imagination PowerVR AXE-1-16M) doesn't have an equivalent mature,
  Buildroot-packaged open driver yet — see
  [Qt5 Application & Rendering](#qt5-application--rendering). This build
  targets Qt5's `linuxfb` QPA plugin with the Qt Quick **software** backend
  instead: no EGL, no Mesa, no DRM userspace, just raster painting straight
  to `/dev/fb0`.
- **The bootloader is a real secure boot chain, not firmware+EEPROM.** AM625
  boots ROM → R5 SPL (`tiboot3.bin`) → TF-A + OP-TEE + A53 SPL (`tispl.bin`)
  → U-Boot (`u-boot.img`) → `extlinux.conf`. See
  [Boot Configuration](#boot-configuration).
- CAN2 data source unchanged: the ODrive USB-CAN adapter, same
  `70-can.rules` udev rule, same `CanBus` C++ code (generic SocketCAN
  `can0`) — zero app-level changes needed for this board.

---

## Hardware Requirements

| Component | Specification |
|-----------|--------------|
| **Board** | BeagleBoard.org BeaglePlay — TI AM625 (quad Cortex-A53 @ 1.4GHz), 2GB or 4GB LPDDR4, onboard eMMC |
| **Display** | HDMI monitor/panel (BeaglePlay's HDMI Type-A, up to 1080p). The same Waveshare-style HDMI+USB-touch panel used on RPi5 should work unchanged. |
| **Boot media** | microSD card (this guide targets SD boot, matching upstream's default `extlinux.conf`) — BeaglePlay's onboard eMMC is an alternative but needs different partition device numbers, see [Open Questions](#open-questions--next-steps-on-real-hardware) |
| **WiFi** | Built-in TI WL1807 (WiLink8) |
| **CAN** | ODrive USB-CAN adapter (same unit as RPi5) — BeaglePlay has no onboard CAN transceiver; its MCAN0 peripheral is exposed on the expansion header but unused by this build |
| **Power** | USB-C |

**Not yet decided/verified**: exact display panel model, whether HDMI EDID
negotiation needs a fallback mode line, touchscreen USB-HID compatibility on
this board's USB controller.

---

## What's Different From RPi5

A quick-reference table for anyone jumping between the two SETUP guides:

| Aspect | RPi5 | BeaglePlay |
|---|---|---|
| Bootloader | RPi firmware + EEPROM config | R5 SPL → TF-A/OP-TEE → U-Boot (real secure boot chain) |
| Toolchain | Buildroot-internal | Buildroot-internal (same reason — Bootlin's toolchain is x86_64-host-only) |
| Display driver | Mesa V3D (open, mature) | tidss (DRM/KMS only, no fbdev-native) |
| GPU | Broadcom VideoCore — Mesa V3D, used | Imagination PowerVR AXE-1-16M — **not used**, see below |
| Qt QPA platform | `eglfs` (KMS + GPU) | `linuxfb` (framebuffer, no GPU) |
| Qt Quick backend | default (GPU-accelerated scenegraph) | `software` |
| WiFi chip | Broadcom BCM43455 (USB-attached-ish, brcmfmac) | TI WL1807 (SDIO, wlcore/wl18xx) |
| WiFi firmware | Manually placed in overlay | Shipped by Buildroot's `linux-firmware` package |
| Boot config file | `config.txt` / `cmdline.txt` | `extlinux.conf` (parsed by U-Boot) |
| Boot partition subdir | `rpi-firmware/` | none — flat, plus `extlinux/` for the conf file |
| Console UART | `ttyAMA10` | `ttyS2` |
| CAN | ODrive USB-CAN adapter | Same |

---

## Host Machine Setup (macOS)

Same as RPi5 — see `SETUP-RPI5.md`'s [Host Machine Setup](SETUP-RPI5.md#host-machine-setup-macos)
section. No BeaglePlay-specific host tooling is needed beyond what's already
there (OrbStack, git, rsync).

---

## Build VM Setup (Ubuntu)

Same VM (`ssh ubuntu@orb`, Buildroot cloned to `~/ultima/buildroot`) can build
both targets — `scripts/setup-vm.sh` is shared across boards per `CLAUDE.md`.

**Additional host build dependencies** confirmed needed for TF-A/OP-TEE beyond
what `setup-vm.sh` installs for the RPi5 kernel/Qt build — `scripts/setup-vm.sh`
now installs these for both boards:

```bash
sudo apt-get install -y python3-pyelftools python3-cryptography \
    device-tree-compiler swig
```

GCC version fix (GCC 14, `HOSTCC=gcc-14 HOSTCXX=g++-14`) applies identically —
see `SETUP-RPI5.md`. Confirmed needed here too: without it, Buildroot's
`host-m4` build breaks the same way on a GCC-15-only Ubuntu image (this VM's
OrbStack default was Ubuntu 26.04, GCC 15-only until `gcc-14`/`g++-14` are
installed explicitly from the `universe` repo).

---

## Project Structure

```
br2-external/
├── configs/
│   └── ultima_beagleplay_defconfig    # Buildroot defconfig
├── board/ultima-beagleplay/
│   ├── extlinux.conf                  # Boot config (kernel/dtb/bootargs) — U-Boot's config.txt equivalent
│   ├── genimage.cfg                   # Static genimage config (boot.vfat + rootfs + data partitions)
│   ├── kernel-fragments.cfg           # Kernel config fragments (CAN, WiFi, touch, display fbdev)
│   ├── post-build.sh                  # Post-build: install extlinux.conf, disable getty, reorder init scripts
│   ├── post-image.sh                  # Post-image: creates data partition, runs genimage
│   ├── patches/                       # Auto-applied via BR2_GLOBAL_PATCH_DIR, same mechanism upstream uses
│   │   └── qt5declarative/            # GCC-14+ cstdint fix — see Qt5 Application & Rendering
│   └── overlay/
│       ├── boot/, data/               # Mountpoints for boot/data partitions
│       ├── etc/
│       │   ├── fstab                  # Identical to RPi5's — board-agnostic
│       │   ├── inittab                # Identical to RPi5's — board-agnostic
│       │   ├── init.d/
│       │   │   ├── rcS                # Identical to RPi5's — init timing wrapper
│       │   │   ├── S00remountro       # Adapted: mmcblk1 partitions, linuxfb env, ttyS2 marker
│       │   │   ├── S11app             # Adapted: linuxfb env
│       │   │   └── S40network         # Adapted: wlcore_sdio instead of brcmfmac
│       │   ├── network/interfaces     # Identical to RPi5's
│       │   ├── wpa_supplicant/wpa_supplicant.conf  # Identical to RPi5's
│       │   └── udev/rules.d/
│       │       ├── 99-input.rules     # Identical to RPi5's
│       │       └── 70-can.rules       # Identical to RPi5's — same USB adapter
│       └── root/.ssh/authorized_keys  # Same key as RPi5
└── package/ultima-app/                # Shared, unmodified — see SETUP-RPI5.md
```

No `br2-external/board/ultima-beagleplay/overlay/lib/firmware/` — WiFi
firmware comes from Buildroot's `linux-firmware` package, not a manual
overlay placement (see [WiFi & Networking](#wifi--networking)).

---

## Buildroot Configuration

**Defconfig**: `br2-external/configs/ultima_beagleplay_defconfig`

### Key settings and why

| Setting | Value | Why |
|---|---|---|
| `BR2_TOOLCHAIN_BUILDROOT_GLIBC=y` | Buildroot-internal toolchain | Same as RPi5; Bootlin's external toolchain only ships x86_64-host prebuilts (invisible on an arm64 build VM) — see status note above |
| `BR2_GLOBAL_PATCH_DIR=".../ultima-beagleplay/patches"` | Auto-applied per-package patches | Same mechanism upstream's `beagleplay_defconfig` uses (`BR2_GLOBAL_PATCH_DIR="board/beagleboard/beagleplay/patches"`) — currently just the `qt5declarative` GCC-14 `cstdint` fix, see [Qt5 Application & Rendering](#qt5-application--rendering) |
| `BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="6.10"` | Kernel version | Matches this Buildroot checkout's own `configs/beagleplay_defconfig` exactly — the actually-tested pin, not a guess |
| `BR2_LINUX_KERNEL_INTREE_DTS_NAME="ti/k3-am625-beagleplay"` | Board DTS | The board's device tree, in-tree in mainline Linux |
| `BR2_TARGET_ARM_TRUSTED_FIRMWARE_CUSTOM_VERSION_VALUE="v2.11"` | TF-A version | Matches upstream's own defconfig |
| `BR2_TARGET_ARM_TRUSTED_FIRMWARE_PLATFORM="k3"` / `_TARGET_BOARD="lite"` | TF-A platform | K3 generation, "lite" board variant used by AM62x |
| `BR2_TARGET_OPTEE_OS_PLATFORM="k3-am62x"` | OP-TEE platform | AM62x-specific OP-TEE OS build |
| `BR2_TARGET_TI_K3_R5_LOADER_CUSTOM_VERSION_VALUE="2024.07"` / `BR2_TARGET_UBOOT_CUSTOM_VERSION_VALUE="2024.07"` | R5-loader / U-Boot version | Matches upstream's own defconfig — the version this Buildroot release's hash files actually expect |
| `BR2_TARGET_TI_K3_R5_LOADER_TIBOOT3_BIN="tiboot3-am62x-gp-evm.bin"` | R5 SPL variant | **GP (General Purpose) security variant** — BeaglePlay ships GP silicon; wrong variant = board won't boot |
| `BR2_TARGET_UBOOT_BOARD_DEFCONFIG="am62x_beagleplay_a53"` | U-Boot board | A53-domain U-Boot defconfig for this exact board |
| `BR2_TARGET_UBOOT_CUSTOM_MAKEOPTS="TEE=$(BINARIES_DIR)/tee-pager_v2.bin"` | U-Boot/OP-TEE wiring | Matches upstream's own defconfig's mechanism for handing U-Boot the OP-TEE binary |
| `BR2_PACKAGE_QT5BASE_DEFAULT_QPA="linuxfb"` | Qt platform | No GPU driver — see [Qt5 rendering](#qt5-application--rendering) |
| `BR2_PACKAGE_LINUX_FIRMWARE_TI_WL18XX=y` | WiFi firmware | Ships onboard WL1807 firmware via Buildroot, no manual overlay needed |

### Full defconfig

See `br2-external/configs/ultima_beagleplay_defconfig` — reproduced comments
there explain each block; not duplicated here to avoid drift between two
copies of the same file.

---

## Kernel Configuration

**File**: `br2-external/board/ultima-beagleplay/kernel-fragments.cfg`

Unlike RPi5's kernel fragment file, this one is mostly **additive only** — it
enables what's needed (CAN, WiFi, touchscreen HID, display fbdev emulation,
module decompression fix) on top of
`BR2_LINUX_KERNEL_USE_ARCH_DEFAULT_CONFIG`'s stock arm64 defconfig, and does
**not** attempt RPi5-style boot-time trimming (disabling KVM, NFS, ftrace,
KASLR, etc.). Those disables were only proven safe on bcm2712 by testing each
one against real hardware one at a time; blindly porting that list onto a
completely different SoC's defconfig risks disabling something AM625/K3
actually needs (e.g. a scheduling/IOMMU feature the K3 boot chain relies on).
Do the same iterative exercise here once more hardware time is available —
see [Open Questions](#open-questions--next-steps-on-real-hardware).

**One disable is already in** (2026-08-07): `CONFIG_DRM_POWERVR` (the
`fd00000.gpu` node's driver, arch-default `=m`) is turned off. This isn't a
speculative trim like RPi5's list — it's a direct fix for hardware evidence
gathered during the `S00remountro`/`fb0`-race debugging below: the unused
PowerVR GPU driver was probing and failing (~2s) before `tidss` got a turn,
directly contributing to the wait `S00remountro` has to background around
the app launch. With the driver not built, nothing probes that node — see
[Init System & Boot Optimization](#init-system--boot-optimization) for the
race this feeds into and [Open Questions](#open-questions--next-steps-on-real-hardware)
item 4 for status.

**Confirmed on real hardware, two boots (2026-08-07):** `dmesg` shows `fb0`
registering at **3.44s**, then **3.58s** on a second reboot (`tidss
30200000.dss: [drm] fb0: tidssdrmfb frame buffer device`), down from the
prior 7.11s/18.06s range, with zero PowerVR/GPU lines in `dmesg` on either
boot — the driver isn't probing, not just failing faster. `ultima-app.log`
shows QML loaded and rendering at 4.83s/5.16s total across the two boots.
Two consecutive tight, consistent runs is good evidence the old boot-to-boot
variance (the genuine `tidss` deferred-probe retry that hit 18.06s once) is
actually gone, not just avoided by luck — this open question is now
considered resolved.

**Required (verified against TI/upstream documentation, not yet against
real hardware)**:
```
CONFIG_MODULE_COMPRESS_NONE=y          # BusyBox modprobe can't handle .ko.xz — same as RPi5
CONFIG_DRM_TIDSS=y                     # AM625 display subsystem driver
CONFIG_DRM_FBDEV_EMULATION=y           # Exposes /dev/fb0 on top of tidss, for Qt's linuxfb plugin
CONFIG_WLCORE=m / CONFIG_WL18XX=m      # Onboard WiFi (SDIO)
CONFIG_CAN_GS_USB=y                    # ODrive USB-CAN adapter — identical to RPi5's fragment
CONFIG_USB_HID / CONFIG_TOUCHSCREEN_USB_COMPOSITE  # USB touchscreen — identical to RPi5's fragment
```

See the file itself for the complete, commented fragment.

---

## Boot Configuration

There is no `config.txt` / `cmdline.txt` equivalent here — AM625 boots
through U-Boot, which reads `extlinux/extlinux.conf` off the boot partition
(the "extlinux" boot protocol, same mechanism Fedora/Debian ARM installers
use).

**File**: `br2-external/board/ultima-beagleplay/extlinux.conf`
```
label ultima-beagleplay
    kernel /Image
    fdtdir /
    devicetree /k3-am625-beagleplay.dtb
    append console=ttyS2,115200n8 root=/dev/mmcblk1p2 rootwait ro rootfstype=ext4 earlycon=ns16550a,mmio32,0x02800000 loglevel=1 quiet logo.nologo vt.global_cursor_default=0 numa=off nokaslr
```

| Parameter | Purpose |
|---|---|
| `root=/dev/mmcblk1p2` | SD card rootfs partition — **assumes SD boot**; eMMC boot would need `mmcblk0p2` instead, see [Open Questions](#open-questions--next-steps-on-real-hardware) |
| `ro` | Read-only root (S00remountro-equivalent pattern, same as RPi5) |
| `earlycon=ns16550a,mmio32,0x02800000` | Early UART console — copied from upstream's known-correct MMIO address for this board's debug UART |
| `loglevel=1 quiet` / `nokaslr` / `numa=off` | Same boot-time-quieting flags as RPi5's cmdline.txt |

### The bootloader chain

Nothing about this resembles RPi5's firmware+EEPROM bootloader. The boot
sequence is:

```
BootROM → tiboot3.bin (R5 SPL, wakeup domain: inits DDR)
        → tispl.bin (TF-A + OP-TEE + A53 SPL, main domain)
        → u-boot.img (U-Boot proper)
        → reads /extlinux/extlinux.conf → loads Image + dtb → boots Linux
```

All four boot-chain components (`tiboot3.bin`, `tispl.bin`, `u-boot.img`,
plus the kernel `Image` and `.dtb`) are Buildroot package outputs, assembled
into `boot.vfat` by `genimage.cfg`. This entire chain — versions, board
defconfig names, TF-A/OP-TEE platform strings — now matches this exact
Buildroot checkout's own `configs/beagleplay_defconfig`
(`~/ultima/buildroot/configs/beagleplay_defconfig` on the build VM), which
is a maintained, working reference for exactly this board. Don't hand-edit
these version pins without diffing against that actual file first — an
earlier version of this defconfig claimed to do so but never actually
compared them, and shipped invented version pins that broke the build (see
the status note at the top of this guide).

### Boot timing: most of it is this chain, not the kernel

Unlike RPi5 (profiled: ~7.8s bootloader / ~1.45s kernel+init+app, see
`SETUP-RPI5.md`), this chain has no equivalent stage-by-stage timing yet —
`dmesg` timestamps only start counting once the kernel itself begins
executing, so none of the `fb0`-timing work above ([Kernel
Configuration](#kernel-configuration)) captures anything before that point.
The onboard status LED (wired to the kernel's `heartbeat` trigger,
`k3-am625-beagleplay.dts:156`) is the only signal available so far for
"kernel has started" without a UART capture, and it showed **~10s of real
elapsed time on power-on before the kernel starts at all** — dwarfing the
~1.5-2s the `dmesg`-visible kernel/app path was taking. Most of the boot
budget is upstream of everything tuned so far.

**One fix landed (2026-08-07):** `CONFIG_BOOTDELAY` in U-Boot's
`am62x_beagleplay_a53` defconfig defaults to `2` — U-Boot waits 2s at every
boot for a keypress to interrupt autoboot before running
`CONFIG_BOOTCOMMAND`. Nothing ever needs to interrupt autoboot on a headless
dash, so this was pure waste. Set to `0` via a new
`br2-external/board/ultima-beagleplay/uboot-fragments.cfg`, wired up the
same way `kernel-fragments.cfg` layers onto the kernel defconfig, via
`BR2_TARGET_UBOOT_CONFIG_FRAGMENT_FILES` in `ultima_beagleplay_defconfig`.
**Confirmed on real hardware**: heartbeat LED now lights at ~8s, matching
the expected ~2s savings almost exactly.

**Second fix landed, no measurable win (2026-08-07):** silenced U-Boot
proper's console (`CONFIG_SILENT_CONSOLE` + `CONFIG_SPL_SILENT_CONSOLE` +
`CONFIG_SILENT_U_BOOT_ONLY` in `uboot-fragments.cfg`, default env
`silent=1` via a patch to `beagleplay.env` — see
`br2-external/board/ultima-beagleplay/patches/uboot/`). Previously U-Boot
printed its full banner and boot-progress lines to the 115200 UART on
every boot even with `bootdelay=0` giving nobody a window to read them.
`CONFIG_SILENT_U_BOOT_ONLY` keeps this scoped to U-Boot itself so the
kernel's own console/earlycon (relied on by `S00remountro`'s UART timing
marker) is untouched — confirmed on real hardware, app still starts
cleanly (3.79s kernel-relative, 1.67s total app startup, no regressions).
**Confirmed on real hardware: no measurable change** — heartbeat LED still
at ~8s. In hindsight this makes sense: suppressing a UART banner at 115200
baud is worth tens of ms at most, well under what a stopwatch-on-LED can
resolve. Not reverted (it's correct and free), but it rules out "boot
console I/O" as a meaningful contributor to the unaccounted gap. Not
wired up for the R5 SPL stage (`ti-k3-r5-loader`) — see the comment in
`uboot-fragments.cfg` for why that's not worth the maintenance cost, and
it wouldn't have mattered anyway given this result.

**Still ~8s unaccounted for, and now more clearly *not* a console-I/O
problem.** The remaining time is somewhere across BootROM → R5 SPL (DDR
training) → TF-A/OP-TEE → U-Boot's own `envboot`/`bootflow scan` — and
right now there's no visibility into which stage(s) actually dominate.
Getting a UART capture on the JST-SH debug header (same physical port as
`console=ttyS2`/`earlycon` — pinout still not confirmed against the board
in hand, see [Debugging](#debugging)) with early boot logging enabled
would tell us whether this is legitimately fixed hardware init (DDR/security
setup, like RPi5's SDRAM training) or has its own slack — see
[Open Questions](#open-questions--next-steps-on-real-hardware).

Falcon mode (R5 SPL jumping straight to TF-A→kernel, skipping A53 SPL +
U-Boot proper) was researched as a bigger potential lever but ruled out for
now: the AM62x-specific wiring for it isn't in our pinned U-Boot 2024.07
(it's an unmerged, TI-SK-board-only patch series posted March 2025 with no
BeaglePlay precedent), it needs TI secure-signing tooling we don't have,
and TI's own numbers put the realistic payoff at ~1-2s — not worth forward-
porting out-of-tree patches for.

---

## Init System & Boot Optimization

Same BusyBox `/etc/init.d/S*` pattern and boot ordering philosophy as RPi5
(see `SETUP-RPI5.md`'s [Init System](SETUP-RPI5.md#init-system--boot-optimization)
section for the full rationale) — **S00remountro launches the app before
udev**, S11app is a stop/restart stub, S40network backgrounds WiFi bring-up.

The only real differences from RPi5's versions:
- Partition devices: `/dev/mmcblk1p1` (boot), `/dev/mmcblk1p3` (data) instead
  of `/dev/mmcblk0p1` / `/dev/mmcblk0p3` — **unverified**, see
  [Open Questions](#open-questions--next-steps-on-real-hardware)
- Qt env vars: `QT_QPA_PLATFORM=linuxfb` + `QT_QUICK_BACKEND=software`
  instead of the `eglfs`/KMS env block
- UART marker device: `/dev/ttyS2` instead of `/dev/ttyAMA10`
- `S40network` loads `wlcore_sdio` instead of `modprobe brcmfmac`
- **`S00remountro` backgrounds a `/dev/fb0` wait (up to 60s) around the app
  launch, instead of waiting synchronously.** First hardware boot
  (2026-08-07) showed `ultima-app.log` failing immediately with "Unable to
  figure out framebuffer device" — `tidss`'s DRM probe is component-based
  (bridges, HDMI DDC/EDID reads over i2c) and hadn't finished by the time
  the pre-udev launch tried to open `/dev/fb0`, unlike RPi5's `vc4` which
  probes fast enough that `eglfs` never needed this. Kernel-side wiring was
  confirmed correct (`tidss_drv.c` calls `drm_fbdev_dma_setup`, `CONFIG_FB`/
  `CONFIG_DRM_FBDEV_EMULATION`/`CONFIG_FB_DEVICE` all enabled), so this is a
  boot-race fix, not a missing-feature fix — but the race turned out to be
  wider than one timeout could reliably cover. Three iterations on real
  hardware:
  1. A 5s synchronous wait — lost the race (`fb0` registered at 8.28s per
     `dmesg`, app launched at 7.11s per `ultima-app.log`).
  2. A 15s synchronous wait — lost the race on a *different* boot, and
     worse than that: `dmesg` showed a genuine deferred probe (`tidss: port
     1 probe failed`, retried later) pushing `fb0` all the way to 18.06s.
     Boot-to-boot variance in the HDMI/DDC probe is real, not just margin
     tuning. The synchronous wait was also quietly delaying `S40network`/
     `S50dropbear` by however long the display took, since `rcS` runs init
     scripts sequentially and nothing after `S00remountro` could start
     until it returned.
  3. Backgrounding the wait+launch as a subshell (`( ... ) &`) fixes both
     problems at once: `rcS` moves on immediately, so WiFi/SSH come up on
     their own schedule regardless of the display, and the app's own wait
     can afford a much longer cap (60s) since it no longer blocks anything
     else. **Confirmed working over SSH on real hardware (2026-08-07)**
     with a manual `/etc/init.d/S11app restart` after `/dev/fb0` existed —
     clean `ultima-app.log` (QML loaded, 0.8s startup), gauge cluster
     rendered on the HDMI display. The backgrounded cold-boot path itself
     — the actual fix for the race — still needs one more hardware
     confirmation pass; the manual restart proved the app/display path
     works, not the new boot-time structure.
  Part of the delay in both failed attempts was the unused PowerVR GPU
  driver (`fd00000.gpu`, no firmware present in Buildroot) probing first
  and burning ~2s failing before `tidss` gets its turn — see
  [Open Questions](#open-questions--next-steps-on-real-hardware) for
  disabling it.

`fstab`, `inittab`, and `rcS` are byte-identical to RPi5's — genuinely
board-agnostic.

---

## WiFi & Networking

Simpler than RPi5 here: BeaglePlay's onboard TI WL1807 (WiLink8, SDIO) is
covered by Buildroot's own `BR2_PACKAGE_LINUX_FIRMWARE_TI_WL18XX` package, so
there's no manual firmware-file placement in the overlay the way RPi5 needed
for its BCM43455 (`brcmfmac43455-sdio.bin` etc. had to be pulled from a
working Raspberry Pi OS install). The kernel driver is `wlcore` + `wl18xx`
(SDIO-attached, not USB), loaded by `S40network` via `modprobe wlcore_sdio`.

`wpa_supplicant.conf` and `network/interfaces` are byte-identical to RPi5's.

---

## Qt5 Application & Rendering

### The app itself

`br2-external/package/ultima-app/` (`Config.in`, `ultima-app.mk`, and all
`src/` files) is completely unmodified — see `SETUP-RPI5.md`'s
[Qt5 Application](SETUP-RPI5.md#qt5-application) section for the full
description of `main.cpp`, `OdoStore`, `CanBus`, and the QML files. Nothing
there is RPi-specific; it's already board-agnostic per `CLAUDE.md`.

**Build-time patch required**: `qt5declarative` needs the same GCC-14+
`cstdint` fix documented in `SETUP-RPI5.md` (`uintptr_t`/`uint32_t` no
longer implicitly available via header transitivity under GCC 14). Applied
here via `br2-external/board/ultima-beagleplay/patches/qt5declarative/0003-qv4compiler-add-missing-cstdint-include.patch`,
picked up automatically by `BR2_GLOBAL_PATCH_DIR` in the defconfig — no
manual step needed, unlike RPi5's guide which still describes creating this
patch by hand each time.

### Rendering: Why Software, Not GPU

RPi5 renders through Qt5's `eglfs` QPA plugin: EGL + GLES2 over Mesa's
V3D driver, direct to `/dev/dri/card1` via KMS. That stack is mature,
fully open-source, and packaged in Buildroot out of the box.

BeaglePlay's GPU is an Imagination Technologies PowerVR Rogue AXE-1-16M.
As of this writing:
- An open-source PowerVR DRM kernel driver landed in **Linux 6.8**,
  developed and tested specifically against the TI SK-AM62 board (same GPU
  as BeaglePlay) — so the kernel side exists and is plausible.
- GLES support, though, requires **Zink** (OpenGL-on-Vulkan) on top of
  Mesa's PowerVR **Vulkan** driver, which only reached conformant Vulkan 1.0
  status in **Mesa 25.3**. Qt5 (which this project uses, not Qt6) talks to
  GPUs exclusively via EGL/GLES — it has no Vulkan backend — so the only path
  to GPU acceleration under Qt5 here is EGL-over-GLES-over-Zink-over-Vulkan.
- That combination (recent-enough kernel + recent-enough Mesa + Zink + EGL
  shim over it, all cross-compiled and packaged inside Buildroot) is not a
  turnkey Buildroot package today. It would need custom package definitions
  and is a real research project of its own.

Given that, this build targets **software rendering**: Qt5's `linuxfb` QPA
plugin (draws straight to a framebuffer device, no EGL/DRM userspace at all)
paired with Qt Quick's `software` scenegraph backend (a raster-based QML
renderer built for exactly this no-GPU case). `/dev/fb0` itself is provided
by `CONFIG_DRM_FBDEV_EMULATION` layered on top of the `tidss` KMS driver —
so the kernel is still doing real KMS mode-setting, Qt just never touches
GPU/EGL to draw into it.

**Performance expectation, unverified**: the gauge cluster is mostly PNG
layer compositing plus needle rotation — not particularly demanding 2D
work — so a quad Cortex-A53 at 1.4GHz doing this in software should be
plausible at a modest resolution and frame rate. This needs to actually be
measured on hardware; if it's not smooth enough, revisit the GPU path above,
or consider dropping resolution/frame rate before chasing the Zink stack.

### Env vars (set by S00remountro)

```
QT_QPA_PLATFORM=linuxfb
QT_QUICK_BACKEND=software
XDG_RUNTIME_DIR=/tmp/runtime
```

No `qt-kms.conf` equivalent — `linuxfb` doesn't take a JSON output config the
way `eglfs_kms` does; it just opens `/dev/fb0` (overridable via the
`linuxfb:fb=/dev/fbN` plugin parameter if the board ever exposes more than
one framebuffer).

**Not yet verified**: whether `linuxfb`'s built-in evdev touch handling picks
up the USB touchscreen automatically the way `eglfs`'s libinput/evdev path
did on RPi5 — worth an early hardware check.

---

## CAN Bus Integration

Identical to RPi5 — see `SETUP-RPI5.md`'s
[CAN Bus Integration](SETUP-RPI5.md#can-bus-integration-syvecs-s7) section
for the full hardware/frame-map/decoding details. The same ODrive USB-CAN
adapter plugs into a BeaglePlay USB port; `70-can.rules` and `CanBus`'s
generic SocketCAN `can0` handling need no board-specific changes.

**Not used**: BeaglePlay's onboard MCAN0 peripheral (exposed on the
expansion header, pins documented in TI's AM62x SDK docs) was considered and
explicitly not chosen for this build — it has no onboard transceiver, so
using it natively would mean wiring an external transceiver board and adding
DT/pinmux work for no real benefit over the USB adapter that already works.
Worth revisiting only if a CAN cape becomes available and the USB adapter
turns out to be a problem.

---

## Display Configuration

HDMI Type-A output, driven by the `tidss` DRM/KMS driver — see
[Qt5 Application & Rendering](#qt5-application--rendering) above for the
`linuxfb`/software rendering rationale.

**Not yet determined**: target resolution/mode. RPi5 targets a fixed
1600x720 ultra-wide panel; whether BeaglePlay drives the same panel over
HDMI, or a different display entirely, needs to be settled before the QML
layout's gauge pivots/angles (calibrated per-resolution — see
`SETUP-RPI5.md`'s [Gauge Needle Alignment](SETUP-RPI5.md#gauge-needle-alignment)
table) can be adapted.

---

## Build Process

### First-Time Build

```bash
# 1. Sync files to VM
rsync -av --exclude='buildroot/' --exclude='.git/' --exclude='output/' \
    ~/code/ultima/ ubuntu@orb:~/ultima/

# 2. Load defconfig
ssh ubuntu@orb "cd ~/ultima/buildroot && make BR2_EXTERNAL=~/ultima/br2-external ultima_beagleplay_defconfig"

# 3. Build (longer than RPi5's first build — the internal toolchain, TF-A,
#    OP-TEE, and U-Boot all build from source; roughly an hour on an 13-core
#    OrbStack VM)
ssh ubuntu@orb "cd ~/ultima/buildroot && make -j\$(nproc) HOSTCC=gcc-14 HOSTCXX=g++-14"

# 4. Copy image
mkdir -p ~/code/ultima/output
scp ubuntu@orb:~/ultima/buildroot/output/images/sdcard.img ~/code/ultima/output/sdcard-beagleplay.img
```

### Quick Build (after initial setup)

Same three steps as RPi5's quick build (sync, `make`, copy) — see
`SETUP-RPI5.md`'s [Build Process](SETUP-RPI5.md#build-process) section;
substitute the image filename above.

### Output

`sdcard.img` with:
- 64MB FAT32 boot partition (`Image`, `.dtb`, `tiboot3.bin`, `tispl.bin`,
  `u-boot.img`, `extlinux/extlinux.conf`)
- 320MB ext4 root partition (read-only)
- 16MB ext4 data partition (persistent odometer state at `/data`)

---

## Flashing

Same `dd`-based approach as RPi5 — see `SETUP-RPI5.md`'s
[Flashing](SETUP-RPI5.md#flashing) section. `scripts/flash-beagleplay.sh`
exists now (adapted from the RPi5 script's disk-picker + `dd`), with one
addition: it detects removable media via `diskutil info`'s "Removable
Media" flag rather than `diskutil list external physical` — a built-in
SD card reader reports as "Internal" even with a card inserted, which the
RPi5 script's detection would miss.

---

## Debugging

Same general approach as RPi5 — `/boot/ultima-app.log`, `dmesg`, `ps | grep
ultima` over SSH once WiFi/dropbear come up; `/tmp/boot-timing.log` for init
script timing (not persisted to `/boot`, matching RPi5's behavior).
`console=ttyS2,115200n8` is BeaglePlay's on-board debug UART per upstream's
own `extlinux.conf` — physical connector/pinout not yet confirmed against
the actual board in hand.

---

## Open Questions / Next Steps on Real Hardware

In priority order — these are the things most likely to need a fix on first
boot:

1. **SD vs eMMC boot device numbering.** This entire config assumes SD card
   boot (`mmcblk1`). If targeting the onboard eMMC instead (no USR button
   held), every `mmcblk1` reference in `extlinux.conf` and `S00remountro`
   needs to become `mmcblk0`.
2. **Touch input under `linuxfb`** — confirm the USB touchscreen is picked
   up without RPi5's `QT_QPA_EGLFS_NO_LIBINPUT=1` equivalent.
3. **Software rendering performance** — measure actual frame rate before
   deciding whether the GPU/Zink path is worth pursuing.
4. ~~**Disable the PowerVR GPU driver** (`fd00000.gpu`)~~ — **Done and
   hardware-verified (2026-08-07).** `CONFIG_DRM_POWERVR` is off in
   `kernel-fragments.cfg` (see [Kernel Configuration](#kernel-configuration)).
   Reflashed and booted: `fb0` now registers at 3.44s (down from 7.11s/18.06s),
   with no PowerVR/GPU lines in `dmesg` at all. See the confirmation note in
   [Kernel Configuration](#kernel-configuration) for the full numbers.
5. **Kernel boot-time trimming** — once the board boots reliably, do the
   same iterative "disable unused subsystem" pass RPi5 got via hardware
   testing (see [Kernel Configuration](#kernel-configuration)).
6. **`scripts/build-beagleplay.sh` / `read-logs-beagleplay.sh`** — not yet
   written; `flash-beagleplay.sh` now exists (see
   [Flashing](#flashing)), adapt the remaining RPi5 equivalents the same way.
7. ~~**U-Boot `bootdelay`**~~ — **Done and hardware-verified (2026-08-07).**
   Was defaulting to 2s; set to 0 via `uboot-fragments.cfg`. Heartbeat LED
   moved from ~10s to ~8s, matching the expected savings. See [Boot timing:
   most of it is this chain, not the kernel](#boot-timing-most-of-it-is-this-chain-not-the-kernel).
8. **Profile the bootloader chain itself (BootROM → R5 SPL → TF-A/OP-TEE →
   U-Boot) via UART capture on the JST-SH debug header.** With `bootdelay`
   gone, ~8s of every boot is still unaccounted for and entirely upstream of
   the kernel — `dmesg` timestamps can't see any of it. This is now the
   single biggest lever left for boot speed, bigger than everything fixed
   so far combined, but needs real stage-by-stage timing data before
   guessing at further fixes (same lesson RPi5's `BOOT_UART=1` profiling
   taught — don't assume a bootloader phase is "fixed hardware init"
   without measuring it first). Needs a USB-UART adapter on the debug
   header; pinout not yet confirmed against the board in hand.
9. ~~**Silence U-Boot's console**~~ — **Done, hardware-verified, no
   measurable win (2026-08-07).** `CONFIG_SILENT_CONSOLE` +
   `CONFIG_SPL_SILENT_CONSOLE` + `CONFIG_SILENT_U_BOOT_ONLY` in
   `uboot-fragments.cfg`, default `silent=1` via a patch to
   `beagleplay.env`. Heartbeat LED still ~8s — banner/status-line UART
   output was only ever worth tens of ms, below stopwatch resolution.
   Kept (it's correct and free) but rules out console I/O as a real
   contributor. See [Boot timing: most of it is this chain, not the
   kernel](#boot-timing-most-of-it-is-this-chain-not-the-kernel).
10. **Falcon mode — researched, ruled out for now.** Would skip A53 SPL +
    U-Boot proper entirely (R5 SPL → TF-A → kernel directly). Not viable
    without a lot of unjustified risk: the AM62x wiring for it isn't in our
    pinned U-Boot 2024.07 (unmerged patch series from March 2025, TI-SK-
    board-only, no BeaglePlay precedent), needs TI's secure-signing tooling
    we don't have, and TI's own numbers put the realistic payoff at ~1-2s.
    Revisit only if the UART profiling (#8) points squarely at the A53
    SPL/U-Boot-proper stage as the dominant cost.

Resolved since the initial scaffolding (see status note at the top): host
build dependencies for TF-A/OP-TEE are confirmed and installed by
`scripts/setup-vm.sh`; the toolchain and bootloader-chain version pins were
corrected and the build now completes end-to-end on the VM. As of
2026-08-07, the board boots on real hardware, WiFi/SSH work, and the Qt app
renders the gauge cluster over HDMI — see the `S00remountro`/`S11app` entry
above and [Init System & Boot Optimization](#init-system--boot-optimization)
for what got fixed to make that happen. HDMI/EDID negotiation came up
without any forced mode needed, resolving the open question that used to be
here.
