# TI Falcon Mode on BeaglePlay — Project Notes

## Goal

Speed up boot time on a physical BeaglePlay board (TI AM62x/AM625 SoC) by
enabling TI **Falcon Mode**: the R5 SPL loads a minimal U-Boot
(`tifalcon.bin`) + kernel FIT image directly from the rootfs ext4 partition,
skipping A53 SPL, U-Boot proper, and GRUB entirely.

- Normal boot: R5 SPL → ATF → OP-TEE → **A53 SPL → U-Boot → GRUB** → Linux
- Falcon boot: R5 SPL → ATF → OP-TEE → Linux (direct)

**Status: done and verified on hardware.** Falcon mode boots correctly;
ATF→kernel window measured at ~0.49s vs ~9.84s baseline (~9.3s faster, of
which ~4.5s is just GRUB's menu timeout and ~5.3s is real A53-SPL/U-Boot/GRUB
overhead eliminated).

**Update (2026-08-08): the actual Ultima Qt gauge-cluster app now runs on
top of this**, falcon boot and all — see "ultima-app integration" and
"Hardware verification" further down. This directory now does more than
just prove falcon mode works in isolation.

## Build environment

- `Dockerfile` + `run.sh` — builds a Yocto/TI SDK (`tisdk`) container.
  Build state lives in a **Docker-managed volume** (`falcon-yocto-build`),
  not a host bind mount — OE-core's sanity checker refuses to run on APFS
  (case-insensitive), and virtiofs bind mounts are too slow for Yocto's I/O
  pattern anyway. `--cap-add SYS_PTRACE` is required or `do_package` fails
  under pseudo/fakeroot.
- Machine: `beagleplay-ti` (A53 build) / `beagleplay-ti-k3r5` (R5 build via
  BitBake multiconfig, `mc:k3r5`).
- To pull built images out of the volume onto the host:
  ```
  docker run --rm -v falcon-yocto-build:/src -v "$(pwd)/deploy:/dst" \
    falcon-yocto:latest bash -c \
    'cp -a /src/tisdk/build/arago-tmp-default-glibc/deploy/images/beagleplay-ti/. /dst/'
  ```

## What was actually wrong upstream

`meta-ti-bsp` (TI's Yocto layer) ships Falcon Mode support, but it's wired
up only for TI's own EVM machine names (`am62xx-evm`, `am62pxx-evm`,
`am62axx-evm`, `am62xx-lp-evm`) — **not** `beagleplay-ti`. Two genuine gaps:

1. **Missing package/config wiring.** `u-boot-ti.inc` only adds the falcon
   package (`u-boot-ti-staging-falcon`, containing `tifalcon.bin`) via
   `PACKAGES:prepend:<evm-machine>`, and only sets
   `UBOOT_CONFIG_FRAGMENTS:ti-falcon` (→ `k3_r5_falcon.config`) in the
   EVM `*-evm-k3r5.conf` machine files. BeaglePlay's machine confs
   (`beagleplay-ti.conf`, `beagleplay-ti-k3r5.conf`) have neither. (The
   falcon boot *logic* itself, `spl_start_uboot`/`CONFIG_SPL_OS_BOOT`, lives
   at the SoC level in `arch/arm/mach-k3/`, not per-board — so this is
   purely a missing-wiring problem, not a missing-feature problem.)

2. **Missing binman devicetree node.** `tools/binman` assembles
   `tifalcon.bin` from a `ti-falcon { insert-template = <&ti_falcon_template>; ... }`
   node. TI's own `k3-am625-sk-binman.dtsi` (am625-sk reference board) has
   one; BeaglePlay's `k3-am625-beagleplay-u-boot.dtsi` does not.

## Fix: custom layer `meta-falcon-beagleplay`

Rather than patching `meta-ti-bsp` in place or hacking `local.conf`, both
gaps were fixed with a small custom layer, mirroring how upstream itself
scopes these variables (recipe/machine-local, never global):

- `sources/meta-falcon-beagleplay/conf/layer.conf`
- `sources/meta-falcon-beagleplay/recipes-bsp/u-boot/u-boot-ti-staging_%.bbappend`
  - `UBOOT_CONFIG_FRAGMENTS:ti-falcon:beagleplay-ti-k3r5 = "k3_r5_falcon.config"`
  - `PACKAGES:prepend:beagleplay-ti = "${FALCON_PKG} "`
  - applies a patch adding the `ti-falcon` binman node
- `sources/meta-falcon-beagleplay/recipes-bsp/u-boot/u-boot-ti-staging/0001-arm-dts-k3-am625-beagleplay-add-falcon-boot-binman-.patch`
  - Host copy of the whole layer (`layer.conf`, the `.bbappend`, and this
    patch): `meta-falcon-beagleplay-src/`. The actual build state (including
    the layer's live copy under `sources/meta-falcon-beagleplay/`) lives only
    in the `falcon-yocto-build` docker volume, not on the host (see "Build
    environment" above) — this directory is a reference copy for reproducing
    it, not something `run.sh`/`Dockerfile` apply automatically. To restore
    it into a fresh or rebuilt volume, copy it to
    `tisdk/sources/meta-falcon-beagleplay/` inside the container and add that
    path to `bblayers.conf`'s `BBLAYERS` (see "Fix" section above).
  - Adds a `ti-falcon` node to `k3-am625-beagleplay-u-boot.dtsi`, mirroring
    `k3-am625-sk-binman.dtsi` but trimmed to the **GP-only** tifsstub
    variant (BeaglePlay doesn't build HS/FS tifsstub images).
  - **Must be a plain unified diff** (`--- a/... +++ b/...`), not git-style
    (`diff --git` / `index 000...`) — quilt (used internally by OE's
    `do_patch`) misreads git-style headers on an already-existing file as
    "create new file" and refuses to apply.
- Layer added to `bblayers.conf`'s `BBLAYERS` list.
- `local.conf` only needed one safe, non-machine-scoped line:
  ```
  DISTROOVERRIDES:append = ":ti-falcon"
  ```

### Why not just put the two `.bbappend` lines in `local.conf`?

Tried that first — `PACKAGES:prepend:beagleplay-ti = "..."` in `local.conf`
caused **174 unrelated fatal QA errors** (`pkgvarcheck`) across completely
unrelated stock recipes (`core-image-sato.bb`, `buildtools-tarball.bb`,
etc.), even under `bitbake -p` (parse-only). Root cause: `beagleplay-ti` is
the *only* machine in the default (non-multiconfig) build, so a
`local.conf` line scoped `:beagleplay-ti` is active during the parse of
*every* recipe in the whole build universe, which broke BitBake's
override-collapse machinery. Bisected with `bitbake -p` + selective
comment-out. Fix: move machine-scoped overrides into the bbappend, scoped
to the one recipe that needs them. Went from 174 errors to 0.

### `DISTROOVERRIDES:append` leading-space gotcha

A heredoc write briefly produced `DISTROOVERRIDES:append = " :ti-falcon"`
(stray leading space before the colon) — would have corrupted the override
list into `"arago :ti-falcon"`. Caught by re-reading the file after
writing; always re-read config files immediately after writing them via
heredoc/shell redirection.

## Build artifacts

- `deploy-falcon/` — falcon-mode image (used going forward):
  - `tisdk-base-image-beagleplay-ti.rootfs.wic.xz` — flashable SD card image
  - `tisdk-base-image-beagleplay-ti.rootfs.wic.bmap`
  - `tiboot3-falcon.bin` — R5 SPL built with `k3_r5_falcon.config` merged in
- `deploy/` — baseline (non-falcon) image, kept for comparison

## Flashing

`./flash.sh <disk> [image.wic.xz]` — accepts `4`, `disk4`, or `/dev/disk4`.
Defaults to `deploy-falcon/tisdk-base-image-beagleplay-ti.rootfs.wic.xz`.
No interactive confirmation prompt (removed on request) — it only refuses
disks `diskutil info` reports as `Internal: Yes`.

**Must be run interactively by the user, not backgrounded by the
assistant** — a `sudo dd` to a raw disk device launched from a
non-interactive/backgrounded shell hung indefinitely with zero progress
(suspected macOS TCC/permission restriction on raw removable-disk access
outside a real TTY session). Foreground runs in an actual terminal complete
normally in ~11s at ~85MB/s for this image size.

## Verification

Serial: `/dev/cu.usbserial-0001` @ 115200 8N1. Capture pattern:
```
exec 3<>/dev/cu.usbserial-0001
stty -f /dev/fd/3 115200 cs8 -cstopb -parenb raw -echo
cat <&3 > logfile
```
(must apply `stty` to an already-open fd for the baud rate to stick).

Falcon boot confirmed via log showing `Loading falcon payload from MMC2` →
`Starting ATF on ARM64 core...` → straight into Linux, with **no**
U-Boot-proper/GRUB text at all (baseline logs clearly show `GNU GRUB
version 2.12`, `Net:` U-Boot ethernet probing, `Loaded env from
uEnv.txt`).

## Boot-time measurement

Used a shared landmark pair present in both logs (ATF-start proxy →
kernel-entry marker) rather than a "true power-on" timestamp, since serial
capture start times aren't perfectly aligned with power-on.

| | ATF → kernel |
|---|---|
| Falcon | ~0.49s |
| Baseline | ~9.84s (~4.47s of which is just the GRUB menu countdown) |

Net: falcon mode saves ~9.3s per boot; ~5.3s of that is genuine
A53-SPL+U-Boot-proper+GRUB-parsing overhead (the rest is GRUB's tunable
timeout, not falcon-specific).

## Board boot-source behavior

- The board still boots from whatever `BOOTMODE`/switch state it's set to;
  holding the **USR** button at boot forces boot from SD card (overriding
  eMMC as the default source) — this needs to be held every time you want
  to boot from the freshly-flashed SD card rather than eMMC, unless eMMC
  itself is reflashed with the falcon image.
- Exact USR-hold timing relative to reset was **not** confirmed against the
  TRM/reference manual — don't state a specific duration without checking
  the datasheet if this matters later.

## Not yet done (optional follow-ups, not requested yet)

- Persist the falcon image to eMMC (`dd` from a running Linux shell on the
  board onto `/dev/mmcblk0`) so it boots falcon by default without holding
  USR.
- Tune/remove the GRUB menu timeout — separate, non-falcon optimization,
  only relevant to the baseline/eMMC image if it's still GRUB-based.

## ultima-app integration (2026-08-08)

Wired the actual Ultima Qt gauge-cluster app into this Yocto build, on top
of the falcon boot work above. `meta-ultima-beagleplay-src/` is a second
custom layer (`meta-ultima-beagleplay`, separate from `meta-falcon-beagleplay`
— unrelated concerns: this one is Qt/app/kernel, that one is U-Boot falcon
wiring), same host-copy-mirrors-volume-copy pattern. `build.sh` syncs it into
the volume and runs `bitbake tisdk-base-image`; run once manually to add
`meta-ultima-beagleplay` (and `meta-qt5`, see below) to `bblayers.conf` if
rebuilding from a fresh volume.

**What was missing, and why**: this Yocto/Arago build had **no Qt5 at all**
— checked every layer already in `bblayers.conf`, zero `qtbase`/
`qtdeclarative` recipes, only an inactive Qt6 dynamic-layer hook. Ultima-app
is qmake/Qt5/QML (`br2-external/package/ultima-app/`, shared and
board-agnostic with the RPi5/Buildroot builds — see UltimaGC's `CLAUDE.md`).
Copying Buildroot's already-built Qt5 5.15.14 libs over was considered and
rejected: Buildroot's toolchain is glibc 2.40, this Yocto build is glibc
2.39 — real risk of runtime symbol-versioning mismatches from mixing
binaries built against different glibc minors. Built Qt5 properly inside
the Yocto tree instead.

- **`meta-qt5`** (community layer, `https://github.com/meta-qt5/meta-qt5`,
  `scarthgap` branch — matches this build's `LAYERSERIES_COMPAT` and meta-ti's
  own Yocto release) added to `sources/` and `bblayers.conf`. Its `qtbase`
  pins Qt **5.15.13** (the `b5.15-shared` branch) — close enough to
  Buildroot's 5.15.14 pin that behavior should match.
- **No usable GPU driver here either** (same PowerVR AXE-1-16M situation as
  the Buildroot BeaglePlay port — see UltimaGC's `SETUP-BEAGLEPLAY.md`
  "Rendering: Why Software, Not GPU"). qtbase's default `PACKAGECONFIG_GL`
  pulls in `eglfs kms gbm gles2` whenever `DISTRO_FEATURES` has `opengl`
  without `x11` (true for arago's distro conf) — overridden via
  `recipes-qt/qt5/qtbase_git.bbappend`, scoped `:beagleplay-ti` only (a
  global override was already proven to break unrelated recipes' QA checks
  earlier in the Falcon work above — same lesson, reapplied), to
  `PACKAGECONFIG:remove = "eglfs kms gbm gles2"` +
  `PACKAGECONFIG:append = "no-opengl linuxfb"`. **Verified**: `qtbase`
  builds clean with this, `qtbase-plugins` contains `libqlinuxfb.so`, no
  `eglfs`/eglfs-adjacent output anywhere in its work directory.
- `QT_QUICK_BACKEND=software` is a runtime env var (set in the systemd
  unit below), not a build-time option — no qtdeclarative PACKAGECONFIG
  change needed for it.

**ultima-app recipe** (`recipes-ultima/ultima-app/ultima-app.bb`): builds
the exact same source tree as the Buildroot RPi5/BeaglePlay builds
(`br2-external/package/ultima-app/src`) rather than a duplicated copy —
bind-mounted read-only into the container at `/home/builder/yocto/ultima-app-src`
(`build.sh`), then copied into `${WORKDIR}` by a `do_unpack:append` python
function before `qmake5`'s `do_configure` runs, so the build never writes
into the shared source tree (mirrors what Buildroot's own `local` site
method already does for this same package — it doesn't build in-place
either). `inherit qmake5` for configure; `do_install` is fully custom since
`ultima-app.pro` has no qmake `INSTALLS`/`target.path` (nothing for
qmake5's own install step to do). Confirmed by reading `main.cpp`: the app
loads `qrc:/main.qml` — everything (QML, PNGs, fonts) is bundled into the
binary via `qml.qrc`'s Qt resource system, so only the single `ultima-app`
binary needs installing, no loose QML/asset files.

**Kernel**: `linux-ti-staging` (6.12.57 here, vs. Buildroot's 6.10 pin) had
`CONFIG_CAN_GS_USB` **not set** — needed for the ODrive USB-CAN adapter.
Added via `recipes-kernel/linux/linux-ti-staging_%.bbappend` +
`ultima-can.cfg` (`CONFIG_CAN=y CONFIG_CAN_RAW=y CONFIG_CAN_GS_USB=y`,
built-in not module, same reasoning as the Buildroot fragment: avoid a
module-load race with early CAN access). `CONFIG_DRM_TIDSS`/
`CONFIG_DRM_FBDEV_EMULATION` were already on by default here — didn't need
touching (`CONFIG_DRM_POWERVR` is still `=m` and unlike the Buildroot port
hasn't been evaluated for its known fb0-probe-race contribution — see
`SETUP-BEAGLEPLAY.md`'s Kernel Configuration section for what that fix
looked like there, not yet ported here).

**Startup**: this rootfs boots via **systemd**, not BusyBox init like the
Buildroot boards (`/sbin/init -> ../lib/systemd/systemd`, confirmed by
checking the built rootfs directly) — a real architectural difference, not
just a config knob. `ultima-app.service`
(`recipes-ultima/ultima-app/files/`) sets `QT_QPA_PLATFORM=linuxfb` +
`QT_QUICK_BACKEND=software`, `Restart=on-failure` + `RestartSec=1` +
`StartLimitIntervalSec=0` so it keeps retrying indefinitely rather than
giving up — this is deliberately standing in for the same `tidss`
DRM-probe race the Buildroot port hit (`/dev/fb0` not ready when the app
tries to open it; see `SETUP-BEAGLEPLAY.md`'s
`S00remountro`/backgrounded-wait writeup), just handled with systemd's own
restart semantics instead of a hand-rolled backgrounded shell loop.
**Not yet hardware-confirmed which approach is faster/more reliable** — the
Buildroot port needed three iterations to get this race right; assume this
needs the same scrutiny once it's actually booting on the board.

**First hardware boot (2026-08-08) found this wasn't a race at all —
`tidss` never loaded, ever.** `ultima-app` crash-looped continuously
(SIGABRT, the classic Qt "no platform plugin could be initialized" abort)
for 300+ seconds straight — `Restart=on-failure` retried dozens of times
and never once recovered, and zero `tidss`/fb0 lines appeared anywhere in
that whole boot log. A genuine race would have self-resolved within a few
seconds once `tidss` finished probing; this didn't, because
`CONFIG_DRM_TIDSS=m` here and nothing was ever loading the module —
udev coldplug apparently wasn't triggering it (never root-caused exactly
why; didn't seem worth the time to chase given a direct fix was available).

**Also found the hard way**: leaving the crash-loop running produced real
`I/O error` / `Buffer I/O error` messages on `mmcblk1p2` (the SD card's
rootfs partition) — journald + likely `systemd-coredump` writing to disk on
every single one of dozens of rapid restarts, on a rootfs that isn't
read-only. Board was powered off immediately; reflashed from a fresh image
rather than trusting that card's state afterward. **Lesson for next time**:
don't leave a write-heavy crash-loop running on a non-read-only rootfs
for minutes — power off fast once a loop is confirmed, don't wait for full
diagnosis with the board still cycling.

**Fix**: tried forcing `CONFIG_DRM_TIDSS=y` in `ultima-display.cfg`
(alongside disabling `CONFIG_DRM_POWERVR`, the proven Buildroot fix for the
same driver-race class of problem) — PowerVR disable took, but
`CONFIG_DRM_TIDSS=y` got silently downgraded back to `=m` by Kconfig
(some dependency forcing it modular — not chased down). Rather than fight
that dependency chain, force the module to load at boot directly:
`/etc/modules-load.d/tidss.conf` (containing `tidss`), installed by
`ultima-app.bb`, plus an explicit `RDEPENDS` on `kernel-module-tidss` so
it can't silently drop out of the image. This sidesteps udev coldplug
entirely rather than trying to fix whatever was wrong with it.
**Not yet hardware-verified this actually fixes it** — image rebuilt, not
yet reflashed/rebooted as of this writing.

**CAN**: `70-can.rules` mirrored byte-for-byte (see the comment in the
copy for why it's a copy and not a shared file — Yocto recipes need files
inside the layer's own `files/`) from
`br2-external/board/ultima-beagleplay/overlay/etc/udev/rules.d/70-can.rules`.
`iproute2` added to `RDEPENDS` since the rule's `RUN+=` lines need `/sbin/ip`
and nothing in this image otherwise pulled it in.

**Data persistence**: `/data/odometer.json` (written by `OdoStore`/`CanBus`,
see `main.cpp`) — for now just a plain directory on the normal rootfs,
**not** the separate read-only-root-plus-writable-`/data`-partition pattern
the Buildroot boards use. This rootfs isn't read-only at all right now.
Revisit if that matters (SD card wear, or if the falcon image ever gets
persisted to eMMC per the follow-up above).

**Status: full `tisdk-base-image` built successfully (all 8177 tasks) with
ultima-app + Qt5/linuxfb + CAN all included, copied to `deploy-falcon/`.
Not yet flashed or hardware-tested.**

One kernel-config false start along the way: `linux-ti-staging`'s
`do_configure` comes from `setup-defconfig.inc`, not the generic
`kernel-yocto` class — it does **not** auto-merge every `*.cfg` fetched via
`SRC_URI`, only what's listed in `KERNEL_CONFIG_FRAGMENTS` (see the comment
in `linux-ti-staging_%.bbappend`). First build had the fragment fetched but
un-merged (`CONFIG_CAN_GS_USB` still unset); fixed by adding it to
`KERNEL_CONFIG_FRAGMENTS` explicitly, then it merged but Kconfig downgraded
the requested `=y` to `=m` (this kernel's `CONFIG_USB` is itself modular) —
accepted as-is, see the fragment file's comment for why that's fine here
(this rootfs boots under systemd/udev normally, unlike the Buildroot boards'
before-udev app launch that needs the driver built in).

**Verified the falcon boot path itself is intact in this build**, not just
that the app compiled: `/boot/tifalcon.bin` and `/boot/fitImage` (both
required by `k3_r5_falcon.config`'s `CONFIG_SPL_FS_LOAD_PAYLOAD_NAME`/
`CONFIG_SPL_FS_LOAD_KERNEL_NAME`) are present in the built rootfs, and
`u-boot-ti-staging-falcon` is in the image's package manifest —
`DISTROOVERRIDES:append = ":ti-falcon"` from the original falcon work is
still in `local.conf`, so this was automatic. Worth understanding why this
works despite the image's `.wks` template being a normal GRUB/EFI
layout (`part --source bootimg-efi --sourceparams="loader=grub-efi"`): that
FAT/GRUB partition is irrelevant to the falcon path. Per
`CONFIG_SYS_MMCSD_FS_BOOT_PARTITION=2` in `k3_r5_falcon.config`, R5 SPL
reads `/boot/tifalcon.bin` + `/boot/fitImage` directly off the **ext4
rootfs partition** via its own built-in ext4 driver — it never touches the
FAT partition or GRUB at all. So the same `tisdk-base-image...wic.xz` this
build always produces is simultaneously a valid falcon-mode SD card
(R5 SPL takes the direct path) — no separate raw-sector placement step,
unlike what the generic (non-K3) U-Boot falcon mode documentation describes.

`dropbear` is in this image by default, but **WiFi is not wired up in this
Yocto build at all** — unlike the Buildroot boards, nothing here ported the
TI WL1807/wlcore/wl18xx firmware+config side, only the kernel CAN fragment.
So SSH only works if BeaglePlay's wired Ethernet comes up with a DHCP lease
(`cpsw`, plausible arago default, not confirmed); otherwise `journalctl -u
ultima-app` needs to happen over the serial console directly. Don't assume
WiFi/SSH access the way the Buildroot bring-up notes do.

## Hardware verification (2026-08-08) — done, working end to end

Flashed `deploy-falcon/tisdk-base-image-beagleplay-ti.rootfs.wic.xz` (the
build with `ultima-app` + Qt5/linuxfb + CAN + the `tidss`
`modules-load.d` fix, see "ultima-app integration" above) to a real
BeaglePlay over serial (`/dev/cu.usbserial-0001` @ 115200 8N1).

**First two boot attempts didn't reach this build at all** — both landed on
the board's existing eMMC content (old stock `U-Boot 2021.01`, unrelated to
this project), because USR wasn't held early/firmly enough through the
actual reset to force SD boot over eMMC (needs to be held *before* power/
reset is applied, not tapped afterward once boot is already underway — the
ROM has usually already picked eMMC by then). Both of those attempts hit
`emergency mode` on the *eMMC's own unrelated content*, not a bug in
anything built here.

**Third attempt, holding USR properly, confirmed everything**:
- `Trying to boot from MMC2` → `Loading falcon payload from MMC2` →
  `Starting ATF on ARM64 core...` → straight into `Linux version
  6.12.57-ti-...` — no GRUB, no tispl.bin/U-Boot-proper text at all. Same
  falcon signature as the original verification at the top of this file.
- `[drm] Initialized tidss 1.0.0 ...` and `tidss ...: [drm] fb0: tidssdrmfb
  frame buffer device` at ~12.3s — the `modules-load.d` fix worked, `tidss`
  loaded and `/dev/fb0` came up.
- **Zero crash/SIGABRT lines this boot** (versus dozens per minute on the
  first hardware attempt, before the `tidss` fix — see "First hardware
  attempt" below).
- `ultima-app.service` showed `Starting`/`Started` twice in the log —
  not chased down further (worth a `journalctl -u ultima-app` check next
  time to see if that's a real single restart or just journal/buffering
  duplication), but not the rapid crash-loop pattern from before either way.
- **Confirmed by eye on the actual HDMI display: the gauge cluster is
  rendering and running.** This is the real end-to-end result — falcon
  boot, Qt5 software rendering via `linuxfb`, and the Ultima app itself,
  all working together on real hardware.

Not yet checked on this hardware: CAN2 data actually arriving (needs the
ODrive USB-CAN adapter connected to the Syvecs S7+), touch input, odometer
persistence across reboots, WiFi (not wired up at all in this build, see
above), boot-time measurement with the app included (the ~9.3s falcon
number above predates the app; re-measuring with the full chain — falcon
kernel boot → systemd → tidss module load → app render — would give the
real end-to-end number this project actually cares about).

### First hardware attempt: the crash-loop and its risk (context, resolved)

Before the fix above, the very first hardware boot of an `ultima-app`-
including image crash-looped continuously (`sig=6`/SIGABRT, Qt's "no
platform plugin could be initialized" pattern) for 300+ seconds — `tidss`
never loaded as a module and `/dev/fb0` never appeared, so `linuxfb` could
never initialize, and `Restart=on-failure` just kept retrying forever
against a permanent (not transient) failure. Left running, this produced
real `I/O error`/`Buffer I/O error` messages on the SD card's rootfs
partition (`mmcblk1p2`) — journald + likely `systemd-coredump` writing to
a non-read-only rootfs on every one of dozens of rapid restarts. Board was
powered off immediately and reflashed fresh rather than trusting that
card's state afterward.

**Lesson for next time a crash-loop shows up on this rootfs**: power off
fast once it's confirmed looping, don't leave it running for extended
diagnosis — this rootfs has no read-only protection the way the Buildroot
boards do (see "Data persistence" above), so a write-heavy loop is a real
risk to the card, not just wasted time.
