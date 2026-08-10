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

**Update (2026-08-09): this all runs from eMMC by default** — power on with
no button held, no SD card inserted, and the board goes ROM → falcon SPL →
kernel → gauge cluster. Verified on hardware: a login prompt flashes for an
instant and the cluster takes the display. See "eMMC boot" below. The SD card
remains a working falcon boot device when USR is held, and is the recovery
path.

**Update (2026-08-09): real power-on-to-gauge-cluster number measured** —
~8.9s, not the ~12.3s eyeballed off a raw log earlier. See "Boot-time
measurement" below and `measure-boot.sh`.

**Update (2026-08-10): optimized further, ~8.9s → ~2.3s to framebuffer**
(**~6.9s to the gauge cluster actually rendering**, a number not previously
measured at all). Root-caused the `CONFIG_DRM_TIDSS` silent-downgrade this
file used to flag as unsolved, quieted the boot console, and trimmed unused
services. See "Boot-time optimization" below.

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
  docker run --rm -v falcon-yocto-build:/src -v "$(pwd)/deploy-falcon:/dst" \
    falcon-yocto:latest bash -c \
    'cp -aL /src/tisdk/build/deploy-ti/images/beagleplay-ti/tisdk-base-image-beagleplay-ti.rootfs.wic.xz \
            /src/tisdk/build/deploy-ti/images/beagleplay-ti/tisdk-base-image-beagleplay-ti.rootfs.wic.bmap \
            /src/tisdk/build/deploy-ti/images/beagleplay-ti/tiboot3.bin /dst/'
  ```
  Note the deploy path is `build/deploy-ti/images/...`, not the
  `build/arago-tmp-default-glibc/deploy/images/...` an earlier revision of this
  file claimed. Most entries there are symlinks, so `cp -aL`, not `cp -a`.

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

- `deploy-falcon/` — falcon-mode image (used going forward; gitignored):
  - `tisdk-base-image-beagleplay-ti.rootfs.wic.xz` — flashable SD card image,
    also what gets written to the eMMC user area
  - `tisdk-base-image-beagleplay-ti.rootfs.wic.bmap`
  - `tiboot3-falcon.bin` — R5 SPL built with `k3_r5_falcon.config` merged in,
    **SD variant** (`mmcdev=1`). This is also the copy baked into the `.wic`;
    the standalone file is only kept for inspection.
  - `tiboot3-falcon-emmc.bin` — same SPL built for eMMC (`mmcdev=0`,
    `bootpart=0:2`), produced by `./build-emmc-spl.sh`. Goes in the eMMC's
    `boot0` hardware partition. See "eMMC boot" below.
- `deploy/` — baseline (non-falcon) image, kept for comparison

## Flashing

`./flash.sh <disk> [image.wic.xz]` — accepts `4`, `disk4`, or `/dev/disk4`.
Defaults to `deploy-falcon/tisdk-base-image-beagleplay-ti.rootfs.wic.xz`.
No interactive confirmation prompt (removed on request) — it only refuses
disks `diskutil info` reports as `Internal: Yes`.

After writing the image it patches the card's MBR disk signature to
`deadbeef`, so the SD and the eMMC (flashed from the same image) can't end up
with the same PARTUUID — see "PARTUUID collision" below for why that
otherwise breaks SD recovery boots in a way that looks like a dead card.

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

### Full power-on-to-gauge-cluster number (2026-08-09)

The ~12.3s figure quoted earlier (in "ultima-app integration") for
kernel-boot-through-framebuffer-ready was eyeballed off a raw, untimestamped
`screenlog.0` capture, not actually measured. `measure-boot.sh` fixes that:
it arms the serial port, timestamps every line as it arrives relative to the
first byte received (t=0, a power-on proxy — same caveat as the ATF/kernel
table above, tight but not a hardware-verified zero), and auto-stops a few
seconds past the `tidss` framebuffer-ready line. Output goes to
`boot-logs/boot-<timestamp>.log` (gitignored).

Three consecutive runs, same board, same falcon image. **Correction (2026-08-10):**
this was actually eMMC-booted, not SD as originally written here — the raw log
shows `Trying to boot from MMC1`, and `k3_r5_falcon_bootmode()` maps
`mmcdev=0`→`MMC1`→eMMC, `mmcdev=1`→`MMC2`→SD. Matters because SD and eMMC have
different raw I/O speed; see "Boot-time optimization" below, which re-measured
on a confirmed-eMMC boot for a true apples-to-apples comparison.

| Run | falcon payload load | ATF start | kernel entry | **framebuffer ready** |
|---|---|---|---|---|
| 1 | 0.171s | 0.918s | 1.419s | **8.859s** |
| 2 | 0.170s | 0.918s | 1.420s | **8.890s** |
| 3 | 0.170s | 0.918s | 1.419s | **8.918s** |

**Power-on → gauge cluster framebuffer ready: ~8.9s, ±30ms** — tight enough
across runs to treat as deterministic rather than jittery. The falcon
boot-loader stage itself (power-on → kernel entry) is a rock-solid ~1.42s
every time; nearly all of the total (~7.5s) is kernel driver probe +
systemd + the `tidss` module load, not the boot loader. wl18xx WiFi
firmware-load failures show up in the log around 10-11.6s (expected — no
WiFi in this build) but land after framebuffer-ready, so they aren't on the
critical path.

Framebuffer-ready is not the same as "app visibly rendering" — there's some
gap between `/dev/fb0` existing and Qt's first frame hitting it that this
method doesn't capture (would need an app-side render-ready log line, which
doesn't exist yet).

## Boot-time optimization (2026-08-10) — ~8.9s → ~2.3s to framebuffer

Three changes, in order of impact:

1. **`CONFIG_DRM_TIDSS` root-caused and fixed.** The prior "not root-caused"
   silent-downgrade (`ultima-display.cfg` requesting `=y`, landing as `=m`)
   was `CONFIG_DRM=m` capping everything downstream in the same Kconfig
   `choice` group. Fix: explicitly force `CONFIG_DRM=y`,
   `CONFIG_DRM_KMS_HELPER=y`, `CONFIG_DRM_TIDSS=y`, `CONFIG_DRM_ITE_IT66121=y`,
   `CONFIG_DRM_DISPLAY_HELPER=y`, `CONFIG_DRM_DISPLAY_CONNECTOR=y`, and
   `# CONFIG_DRM_POWERVR is not set` (the competing choice member) in
   `ultima-display.cfg`. Verified against the built `.config`. This also let
   `kernel-module-tidss` + `/etc/modules-load.d/tidss.conf` be deleted
   entirely (`ultima-app.bb`) — tidss is now genuinely built-in, no module to
   force-load. This is the dominant win: tidss init moved from kernel_ts
   ~7.8s to **~0.5s**.
2. **Quiet boot.** Synchronous printk to a 115200-baud UART is real critical-path
   cost, not just log noise — measured ~38KB of console text by the old
   tidss-ready point, ~3.3s of pure UART wire time. The kernel's own
   `CMDLINE_EXTEND` doesn't exist on this kernel/arch (arm64 only has
   `CMDLINE_FROM_BOOTLOADER`/`CMDLINE_FORCE`), and a static `CONFIG_CMDLINE`
   can't work either (this image boots from both SD and eMMC with different
   PARTUUIDs, fixed up at runtime). Fix: patch U-Boot's
   `k3_falcon_fdt_fixup()` directly (`arch/arm/mach-k3/common.c`) to append
   `quiet` to the falcon cmdline it already generates —
   `meta-falcon-beagleplay-src/recipes-bsp/u-boot/u-boot-ti-staging/0002-*.patch`.
   Side effect: `measure-boot.sh`'s serial-log landmark regexes for "kernel
   entry" (`Linux version`) and "framebuffer ready" (`tidssdrmfb`/`Initialized
   tidss`) no longer appear on the console at all — those printks are exactly
   what `quiet` suppresses. Post-boot numbers now come from SSH
   `dmesg`/`journalctl -o short-monotonic` instead (kernel ring buffer keeps
   everything regardless of console level).
3. **Trimmed 5 unused services** (`docker-moby`, `containerd-opencontainers`,
   `lldpd`, `netperf`, `systemd-telnetd`) via `SYSTEMD_AUTO_ENABLE:${PN} =
   "disable"` bbappends (`meta-ultima-beagleplay-src/recipes-ultima/boot-trim/`).
   `psplash` also disabled the same way. `docker.socket` resisted the same
   fix (a higher-priority TI-layer bbappend competes) — not chased further,
   socket activation means near-zero boot cost anyway.

**Considered and reverted:** gating `ultima-app.service` on `dev-fb0.device`
via `Requires=`/`After=` to fix the crash-loop race noted in "ultima-app
integration" below. `Requires=` on a `.device` unit does not wait for it to
appear — it fails outright if not already active, which would have shipped a
cluster that never starts (worse than the crash-loop). Reverted; tidss now
being built-in makes fb0 exist before systemd starts probing it, which
appears to have made the race moot (zero SIGABRT across all eMMC/SD runs
since). Re-open only if a boot log ever shows the SIGABRT again.

### Results, confirmed-eMMC, same power-on-proxy methodology as the original baseline

Root device confirmed both times via the raw serial log's `Trying to boot
from MMC1` (see correction above) / `findmnt -no SOURCE /` on the board.
Kernel-relative timestamps converted to wall time via an offset derived from
two independent anchor points in the same boot's serial log (both agreed to
the millisecond: +1.492s for this run).

| Landmark | Before | After |
|---|---|---|
| `/dev/fb0` ready | ~8.9s | **~2.29s** |
| Gauge cluster actually rendering | not previously measured | **~6.94s** |

The old metric only covered fb0 existing, not the app visibly rendering; the
new number is more complete (full Qt/QML startup included) and still beats
it by ~2s. The dominant remaining cost shifted from tidss (fixed) to
systemd's own path to starting `ultima-app.service` (kernel_ts 0.8s → 4.0s,
~3.2s on eMMC vs ~6.9s for the same gap on SD in an interim test — eMMC's
faster I/O matters a lot here). That gap is the next lever if further
optimization is wanted.

Caveat: this run's bootloader stage (payload-load → ATF-start) measured
~0.4s slower than the original 3-run baseline table (1.324s vs 0.918s to
ATF-start) — the reset was triggered via `reboot` over SSH rather than a
physical power cycle, and the log shows a watchdog-disable delay right
before SPL starts that the power-cycled baseline wouldn't have hit. Doesn't
affect the post-kernel-entry numbers above.

### `emmc-install.sh` bugs found and fixed while re-flashing eMMC for this test

- `sig()` used `od -An -tx1` (GNU-only flags) to compare eMMC/SD disk
  signatures. This board's BusyBox `od` rejects `-A`/`-t` outright; both
  sides of the comparison silently read as empty strings, which compare
  equal and falsely trigger the "signatures collide" branch every time.
  Fixed: `hexdump -e '4/1 "%02x"'` (BusyBox supports this). No data was
  corrupted by the false positive — the collision-avoidance branch just ran
  unnecessarily and repatched the SD's disk signature, which is
  self-correcting (PARTUUID is read fresh from the live partition table at
  every falcon boot).
- The partition/superblock size check required exact equality
  (`PART_KB = FS_KB`), but real SD/eMMC media of the same nominal size
  commonly differ by a few KB in actual sector count, and the `.wic`'s last
  partition is sized "rest of disk" at build time — so exact equality
  doesn't hold across different physical media even for a good write
  (confirmed: this eMMC's partition came out 2KB larger than its
  superblock). The actual hazard the check guards against is a superblock
  *larger* than its partition ("bad geometry", unbootable). Fixed:
  `[ "$FS_KB" -le "$PART_KB" ]`.

## Board boot-source behavior

- Default (no button): boots from **eMMC** — now falcon mode with the gauge
  cluster on it, see "eMMC boot" below.
- Holding the **USR** button forces boot from the SD card instead. This is the
  recovery path; keep the SD bootable, because repairing the eMMC rootfs
  requires booting something else.
- **USR must be held for the entire boot sequence, not just tapped at
  power-on.** It must be down *before* power/reset is applied and stay down
  for a couple of seconds after. Empirically, releasing it partway through
  makes the LEDs go fully dark (looks like a power loss, not a fallback to
  eMMC) — hold until a stable login prompt, or at least ~15-20s. Tapping it
  late silently boots stale eMMC content instead, which looks like a real
  failure but isn't.
- Exact USR-hold timing relative to reset was **not** confirmed against the
  TRM/reference manual beyond the above — don't state anything more specific
  without checking the datasheet.

## Not yet done (optional follow-ups, not requested yet)

- Tune/remove the GRUB menu timeout — separate, non-falcon optimization,
  only relevant to the baseline image if it's still GRUB-based.

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

**Superseded (2026-08-10):** the module-load workaround above worked but left
the real Kconfig bug unfixed. Root-caused later — `CONFIG_DRM=m` was capping
the whole choice group, not something deeper — and fixed directly, so
`CONFIG_DRM_TIDSS` is now genuinely built-in and the module-load-and-RDEPENDS
workaround was removed entirely. See "Boot-time optimization" further down.

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

## Dash clock doesn't persist a manual set (2026-08-10) — root-caused on hardware

Reported symptom: SetTimeScreen's UI works (steppers, AM/PM, Save all
respond) but the new time "doesn't save." Checked live over SSH rather than
guessing — two independent, stacked causes, both real:

1. **`systemd-timesyncd` is enabled by default in this base image** and was
   actively NTP-synced (`timedatectl`: `System clock synchronized: yes`,
   `NTP service: active`, polling `1.pool.ntp.org` over the wired Ethernet
   used for SSH/bench access). `SystemClock::setTime()` calls
   `clock_settime()` directly — it doesn't go through
   `org.freedesktop.timedate1`, so systemd-timesyncd has no idea a manual
   override happened and never disables itself the way `timedatectl
   set-time` would. Net effect: NTP just wins the race, repeatedly, any
   time the board has network — which is bench-only; the deployed car has
   none (see CLAUDE.md). Not a loss for the real use case to turn off, since
   NTP was never a legitimate clock source there anyway.
2. **Nothing loaded the RTC's time back into the system clock at boot.**
   `/dev/rtc0` (`bq32k 0-0068`) exists and — confirmed with `hwclock -r` —
   correctly held a previously-written time, so `SystemClock`'s
   write-through to hardware was already working. But
   `CONFIG_RTC_HCTOSYS` is unset in this kernel's `.config`, and even
   setting it wouldn't help: `CONFIG_RTC_DRV_BQ32K=m` here (module, not
   built in — same Kconfig-driven `=y`→`=m` downgrade pattern as
   `CONFIG_CAN_GS_USB`, see `ultima-can.cfg`), so the driver isn't bound yet
   at the kernel's early-boot HCTOSYS point regardless. No udev rule or
   systemd unit filled the gap either — `util-linux-hwclock` is installed
   (`/usr/sbin/hwclock` present) but ships no service of its own. Added
   `recipes-ultima/ultima-hwclock-load`, running `hwclock --hctosys`.

Both are needed — (1) alone still loses the RTC's value on the very next
disconnected boot; (2) alone still gets overwritten live any time the board
is on the bench with Ethernet plugged in.

**First attempt at both, hardware-verified wrong — two more lessons:**

- **`SYSTEMD_AUTO_ENABLE:pn-systemd-timesyncd = "disable"` (in a
  `boot-trim/systemd_%.bbappend`, matching this layer's existing pattern for
  docker-moby/psplash/etc.) was a silent no-op.** Flashed, booted, checked:
  `systemctl is-enabled systemd-timesyncd` still said `enabled`, and a real
  `.wants` symlink existed. `opkg list-installed` showed why —
  `systemd-timesyncd.service` ships inside the base `systemd` package here,
  not split into its own `systemd-timesyncd` sub-package, so
  `SYSTEMD_AUTO_ENABLE:pn-<pkg>` had no package named `systemd-timesyncd` to
  attach to (confirmed by reading `systemd_populate_packages()` in
  `systemd.bbclass` — the enable/disable decision is resolved per *package*,
  from whichever package actually declares `SYSTEMD_SERVICE:<pkg>`, not per
  service name). A package-level override on `systemd` itself would've been
  too broad — that package's `SYSTEMD_SERVICE` list covers more than just
  timesyncd. Fixed by masking the unit directly in the finished rootfs
  instead (`ROOTFS_POSTPROCESS_COMMAND` in `tisdk-base-image.bbappend`,
  `ln -sf /dev/null .../systemd/system/systemd-timesyncd.service` after
  removing the `.wants` symlink) — surgical, and it runs after every
  package's own postinst so ordering can't undo it.
- **`ultima-hwclock-load.service`'s `ConditionPathExists=/dev/rtc0` also
  silently no-op'd, every boot, on real hardware** — confirmed in
  `journalctl -b`: the condition check ran and failed at kernel timestamp
  4.54s, `bq32k` didn't register as `rtc0` until 5.11s, well under a second
  later. `ConditionPathExists` skips immediately on a false read, it doesn't
  wait or retry — same trap as the `Requires=dev-fb0.device` race
  `ultima-app.service`'s own history already warns about (see its
  `[Unit]` comment). Replaced with a bounded poll loop in `ExecStart`
  (0.1s × up to 30, i.e. ≤3s, no-ops past that). Also dropped this unit's
  `Before=ultima-app.service` entirely rather than trying to tighten the
  ordering further — with boot-to-framebuffer this project just cut from
  ~8.9s to ~2.3s (see "Boot-time optimization" above), nothing about
  restoring the clock should ever sit in front of the app's start. It runs
  concurrently now; the dash clock re-reads system time every second on its
  own (`clockText`'s `Timer` in `main.qml`), so on the rare boot where this
  loses the race the display is briefly stale and self-corrects a moment
  later — a harmless cosmetic blip, not worth trading app-start latency for.

Both corrected and re-flashed; re-verified on hardware after the corrected
build: `systemd-timesyncd` shows `masked`/`inactive`, `NTP service: n/a`;
`ultima-hwclock-load` ran successfully (journal shows the clock jump
mid-unit — "Starting" logged under the stale boot-default time, "Finished"
under the RTC-corrected one); `timedatectl` matched the RTC exactly with no
network sync involved. Root-caused, fixed, and confirmed — not just built.

## Glyph fix looked broken on hardware — wasn't Canvas, was stale sstate

Separately, the glyph fix (see main session notes / git log for
SetTimeScreen.qml + main.qml — replaced Text glyphs the target has no font
coverage for with Canvas-drawn icons) tested as "just outlined rectangles"
on real hardware after two full image rebuilds, even though it rendered
correctly in a macOS dev build first. Before touching the Canvas code,
checked whether the new QML had even reached the board — it hadn't:

**`ultima-app.bb`'s `do_unpack` signature is computed from `SRC_URI` alone
(just `ultima-app.service` + `70-can.rules`) — nothing hashes the contents
of `ULTIMA_APP_EXTERNAL_SRC`, the bind-mounted external source tree the
`do_unpack:append` python actually copies from.** A source-only edit (QML,
C++, anything under `br2-external/package/ultima-app/src`) leaves every
task's signature unchanged, so bitbake reuses the stale sstate object and
silently ships the OLD binary. Confirmed directly: two full image builds in
a row after editing the QML produced zero "ultima-app" task lines in either
build log, and the work directory's copy of `SetTimeScreen.qml` had none of
the new code in it. Same class of trap as the `KERNEL_CONFIG_FRAGMENTS` one
this layer already learned the hard way (`linux-ti-staging_%.bbappend`) —
a task's signature only reflects what bitbake was told to track, not
everything the task's script actually touches at runtime.

Fixed with `do_unpack[nostamp] = "1"` on the recipe — forces `do_unpack` and
everything downstream (`do_compile`, `do_install`, `do_package`, ...) to run
on every single build, which is what "always mirror the current host
source" was already supposed to mean. Confirmed the fix by checking task
logs for actual "Started"/"Succeeded" lines this time, not just build exit
code — a green build here proves nothing about whether the app recipe
itself ran.

**Fast iteration loop discovered along the way, worth reusing before
reaching for a full image build + SD flash + USR-held reboot:**
```
./build.sh ultima-app        # builds just the recipe (~1min once source-tracked)
docker run --rm -v falcon-yocto-build:/src -v /tmp/hotdeploy:/dst falcon-yocto:latest \
  cp /src/tisdk/build/arago-tmp-default-glibc/work/aarch64-oe-linux/ultima-app/1.0/image/usr/bin/ultima-app /dst/
scp /tmp/hotdeploy/ultima-app root@beagleplay-ti.local:/root/ultima-app.new
ssh root@beagleplay-ti.local 'mv /root/ultima-app.new /usr/bin/ultima-app; systemctl restart ultima-app'
```
Overwriting `/usr/bin/ultima-app` in place while it's running fails with
`ETXTBSY` (scp's sftp backend reports it as a generic "dest open" failure) —
upload to a temp path and `mv` over it instead; renames don't hit that,
since the running process keeps its own reference to the old inode. This
whole loop is minutes and needs zero physical access to the board, versus a
full image build + card swap + held-button power-on. Doesn't help for
kernel/DT/bootloader changes, only app source.

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
above). Boot-time measurement with the full chain is now done — see
"Boot-time measurement" → "Full power-on-to-gauge-cluster number" above
(~8.9s power-on to framebuffer-ready, measured with `measure-boot.sh`, not
the eyeballed ~12.3s this paragraph used to cite).

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

## eMMC boot — solved (2026-08-09)

The board boots falcon mode, and therefore the gauge cluster, straight from
eMMC with **no USR button held**. The SD card remains a working falcon boot
device when USR *is* held, and is the recovery path — keep it that way, since
repairing the eMMC rootfs requires booting something else.

Three separate things all have to be right, and each fails in its own
distinctly-diagnosable way:

1. A bootloader in the eMMC's **`boot0` hardware partition**. The TI K3 ROM
   does not look in the user area for eMMC the way it does for SD.
2. That bootloader must be a **separate SPL build targeting eMMC** — falcon's
   boot device is compile-time, from two env vars, not from wherever the ROM
   happened to load the SPL.
3. A **valid filesystem in the eMMC user area**, whose ext4 superblock size
   matches the partition size.

### Procedure

**No serial console is needed for any of this** — the board is reachable over
SSH (see "Board access over SSH" below), which is how the files and commands
get there.

```
./build-emmc-spl.sh                  # once per u-boot rebuild
./flash.sh 4                         # SD card, gets a unique disk signature
                                     # boot the board with USR held
./emmc-push.sh                       # copies everything over SSH and runs it
```
then remove the SD card and power on with USR **not** held.

`emmc-push.sh` refuses to run unless the board is actually booted from the SD
(`findmnt` says `mmcblk1p*`), since overwriting the eMMC from a system running
on it is exactly the mistake worth preventing. It ends with a clean
`sync; poweroff`.

`emmc-install.sh` does the eMMC side and verifies every step — md5 of the
transferred files, that the SPL really is the `mmcdev=0` variant, image written
and read back off the media with caches dropped, superblock size vs partition
size, boot0 content vs source, SD/eMMC signature distinctness — stopping at the
first failure rather than leaving a half-installed eMMC. Its log is
`/root/emmc-install.log` on the board.

If the board has no network, `./emmc-serve.sh` serves the same files over HTTP
and prints a serial-console one-liner instead. Even then the files move over
the network rather than being typed — see "Serial console caveats" below; past
~80-90 characters the console reliably drops and duplicates characters.

**Provenance / what is and isn't script-verified.** The boot is
hardware-verified: confirmed 2026-08-09 booting to the gauge cluster from eMMC
with the SD card removed and no button held. The eMMC was populated by hand
first (in the standalone `~/code/falcon` working tree, whose build shares this
repo's `falcon-yocto-build` Docker volume, so the artifacts are the same bits).

- `build-emmc-spl.sh` — **run and verified against the live board.** Its
  output's SPL payload (everything past the x509 cert) is byte-identical to
  what is sitting in the running board's eMMC `boot0` and booting it today:
  ```
  # board                                    # Mac
  head -c 273749 /dev/mmcblk0boot0 |         tail -c +1268 \
      tail -c +1268 | md5sum                     deploy-falcon/tiboot3-falcon-emmc.bin | md5
  # both: 0609b690b4fbf1b89031332fc19b0ec6
  ```
- `flash.sh` — **run and verified end to end**, including the new disk-signature
  patch. A card flashed with it booted the board, and the running kernel
  reported `root=PARTUUID=deadbee5-02` while the eMMC stayed on `076c4a2a-02`.
  That is also the empirical proof that patching the *card* is sufficient and
  the `.wic` needs no patching: `k3_falcon_fdt_fixup()` read the patched
  signature off the live partition table at boot.
- `emmc-push.sh` — preflight and the "refuse unless booted from SD" guard are
  live-tested in both directions (refused against an eMMC-booted board, and the
  guard's `case` accepts the real `/dev/mmcblk1p2`). The install it drives has
  not been run end to end.
- `emmc-install.sh` / `emmc-serve.sh` — codify the manual procedure with
  verification added, but **have not been run**, because the eMMC was already
  correct by the time they existed. Their parsers (`sig()`, the
  `/proc/partitions` and `dumpe2fs` awk) were tested against real data under
  busybox; every tool they use is confirmed present on the board; and the fd-3
  progress channel is confirmed to survive a non-interactive `ssh host 'sh
  script'`. Still: read `/root/emmc-install.log` the first time rather than
  assuming a silent success.

### Non-destructive verification pass (2026-08-09)

Run from an SD-booted board over SSH, with the eMMC deliberately left alone —
enough to exercise everything except the eMMC write itself:

| Check | Result |
|---|---|
| `findmnt -no SOURCE /` | `/dev/mmcblk1p2` — genuinely the SD |
| SD PARTUUID | `deadbee5-02` (flash.sh's patch, live) |
| eMMC PARTUUID | `076c4a2a-02` — distinct, no collision |
| `/proc/cmdline` | `root=PARTUUID=deadbee5-02` |
| eMMC filesystem | `clean`, 3794688 blocks — untouched |
| eMMC `boot0` SPL | still `mmcdev=0` / `bootpart=0:2` — untouched |
| `ultima-app` | `active`, cluster on the display |

Note the board reaches the cluster roughly half a minute before it answers on
the network — the app does not wait for DHCP. "No SSH yet" right after boot
means wait, not broken.

### On rebuilding the eMMC SPL: it won't be byte-identical

`tiboot3.bin` is a 1267-byte x509 certificate followed by the SPL image. Two
builds of identical source differ in ~94 bytes, all inside that certificate —
the serial number (offsets 16-35) and the signature (1204-1263). The SPL
payload after the certificate is bit-for-bit reproducible, and the banner
string is too (`SOURCE_DATE_EPOCH` pins the build timestamp). This is a GP
(General Purpose) device, so the ROM doesn't verify the signature; a differing
cert is expected and harmless. Compare payloads, not whole files:

```
python3 -c "
d=open('a.bin','rb').read(); r=open('b.bin','rb').read()
print(d[1267:]==r[1267:])"
```

### Falcon's boot device is compile-time — and it's TWO env vars

Falcon mode does **not** use whichever device the ROM loaded the SPL from.
`arch/arm/mach-k3/am62x/am625_init.c` routes `spl_boot_device()` through
`k3_r5_falcon_bootmode()` (`arch/arm/mach-k3/r5/common.c`) when
`CONFIG_SPL_OS_BOOT_SECURE` is set, and that reads the **environment**:

```c
int k3_r5_falcon_bootmode(void)
{
	char *mmcdev = env_get("mmcdev");
	if (!mmcdev) return BOOT_DEVICE_NOBOOT;
	if (strncmp(mmcdev, "0", sizeof("0")) == 0) return BOOT_DEVICE_MMC1;      // eMMC
	else if (strncmp(mmcdev, "1", sizeof("1")) == 0) return BOOT_DEVICE_MMC2; // SD
	else return BOOT_DEVICE_NOBOOT;
}
```

There is **no writable environment** in this build (every `CONFIG_ENV_IS_IN_*`
is unset, `CONFIG_ENV_SIZE=0x20000` notwithstanding), so this is baked in at
compile time from `board/beagle/beagleplay/beagleplay.env` — which is
board-specific, so editing it doesn't affect TI's other AM62x boards (unlike
the shared `board/ti/am62x/am62x.env`).

Setting `mmcdev=0` alone is **not enough**. It gets as far as:

```
Trying to boot from MMC1          <- correct, eMMC
Loading falcon payload from MMC1  <- payload loaded fine
MMC: no card present
** Bad device specification mmc 1 **
k3_falcon_fdt_fixup: Failed to get part details for mmc 1:2 [-19]
```

because `k3_falcon_fdt_fixup()` (`arch/arm/mach-k3/common.c`) separately reads
`boot` and `bootpart` to derive the kernel's root PARTUUID:

```c
strlcpy(bootmedia, env_get("boot"), sizeof(bootmedia));    // "mmc"
strlcpy(bootpart, env_get("bootpart"), sizeof(bootpart));  // "1:2" <- still SD
ret = blk_get_device_part_str(bootmedia, bootpart, &dev_desc, &info, 0);
snprintf(str, ..., "console=%s root=PARTUUID=%s rootwait", ..., info.uuid);
```

So **both** must change together:

| `beagleplay.env` | SD (default build) | eMMC |
|---|---|---|
| `mmcdev` | `1` | `0` |
| `bootpart` | `1:2` | `0:2` |

`build-emmc-spl.sh` flips them, recompiles just the R5 SPL
(`bitbake -c compile -f mc:k3r5:u-boot-ti-staging`), extracts
`tiboot3.bin` → `deploy-falcon/tiboot3-falcon-emmc.bin`, then puts the source
back and recompiles again so the volume is left producing the SD variant. It
asserts that last part rather than assuming it — silently deploying an
eMMC-targeting SPL into an SD `.wic` produces exactly the confusing
"`MMC: no card present`, from a card that is plainly present" failure above.

Note the multiconfig scoping: the R5/falcon SPL lives entirely under the
`k3r5` BitBake multiconfig (`meta-ti-bsp/conf/multiconfig/k3r5.conf`,
auto-enabled via `BBMULTICONFIG` in `mc_k3r5.inc`). Unscoped
`u-boot-ti-staging` / `virtual/bootloader` targets build the unrelated A53
U-Boot instead — `bitbake -c cleansstate virtual/bootloader` cleans the wrong
thing and looks like it did nothing.

### Which `tiboot3.bin` goes where

Easy to mix up, and a mismatch fails in a confusing way:

| Location | Variant | `mmcdev` / `bootpart` | Host file |
|---|---|---|---|
| SD FAT partition (`tiboot3.bin`) | SD | `1` / `1:2` | built into the `.wic` image |
| eMMC `boot0` | eMMC | `0` / `0:2` | `deploy-falcon/tiboot3-falcon-emmc.bin` |

Reflashing the SD only replaces the SD variant; the eMMC `boot0` copy is
untouched by `flash.sh` and survives. Check any given file with
`strings tiboot3.bin | grep -E '^mmcdev=|^bootpart='`.

### The bug that cost the most time: `tifalcon.bin` is not `tiboot3.bin`

Worth reading before touching eMMC boot again, because it burned two sessions
and produced a confident, wrong "needs JTAG" conclusion.

Symptom: eMMC-only boot (SD removed, USR not held) failed completely — no
heartbeat, no LEDs, i.e. stuck before Linux — with serial showing one repeating
frame, identical each time and padded out with `C` (0xCC) bytes:

```
01000000011a0000616d36327800000000000000475020200100010001000100CCCC...
```

That decodes to ASCII `am62x` and `GP ` (General Purpose device type): the K3
ROM's own boot notification frame, looping because the ROM found no valid image
and fell back to waiting for a UART image transfer.

Root cause: the file being written into `boot0` was **`/boot/tifalcon.bin`**,
the falcon *payload* (the minimal U-Boot that SPL loads and hands off to,
~995KB), not **`tiboot3.bin`**, the R5 SPL the ROM actually loads (~274KB,
x509-wrapped). The ROM rejected it every time. The tell is right there in the
byte counts — any real `tiboot3.bin` in this build is 273,749 bytes.

Everything the failing sessions chased instead was a dead end, and all of it is
now **ruled out**, so don't re-investigate:

- `PARTITION_CONFIG` (EXT_CSD byte 179) ships as `0x48` — boot partition 1
  enabled, boot-ack on. `mmc bootpart enable 1 1 /dev/mmcblk0` is a no-op.
- `BOOT_BUS_CONDITIONS` (byte 177) ships as `0x02` — x8, reset-to-x1-after-boot,
  SDR-backward-compatible. `mmc bootbus set ...` is a no-op.
- Writing `boot1` as well as `boot0` is unnecessary; `PARTITION_CONFIG=0x48`
  selects the partition Linux exposes as `mmcblk0boot0`.
- "boot0 content doesn't survive power cycles" — the observed corruption was a
  ~995KB file compared against itself across sessions where the wrong thing was
  being written anyway. There is no evidence of a boot-partition endurance
  problem.
- BeagleBoard's own docs show `mmc bootpart enable 1 2 /dev/mmcblk0`; on this
  board's mmc-utils the syntax is `enable <boot_partition> <send_ack> <device>`
  with `send_ack` only ever 0 or 1. The doc's `2` is a different version's
  semantics — don't copy it literally.

`mmc-utils` isn't in TI's opkg feed; it's in the image because
`tisdk-base-image.bbappend` adds it to `IMAGE_INSTALL`.

Note `cmp file /dev/mmcblk0boot0` always exits 1 with "EOF on file" because the
device is larger than the image — that is *not* a failure. Use
`head -c <exact size>` to get a meaningful exit code (`emmc-install.sh` does).

### PARTUUID collision between SD and eMMC

Both devices get flashed from the same image, and a Linux MBR PARTUUID is
derived from the 4-byte disk signature at file offset 440 (`0x1B8`) — which the
wic builder emits **deterministically, not randomized per flash** (confirmed by
forcing a `bitbake -c image_wic -f tisdk-base-image` rebuild and diffing:
byte-identical). So flashing both gives both the identical
`PARTUUID=076c4a2a-02`.

The kernel's `root=PARTUUID=...` takes the first matching device, and eMMC
(`mmc0`) finishes async probing before SD (`mmc1`), so on a collision an
SD-forced recovery boot mounts the eMMC's rootfs — or panics with `Unable to
mount root fs on "PARTUUID=076c4a2a-02"` if it isn't ready — **on every boot,
regardless of SD card health**. This produced a very convincing fake "the SD
card is dead" regression.

Fix: `flash.sh` patches every freshly-flashed SD card's disk signature to
`deadbee5` after writing the image, and `emmc-install.sh` independently
compares the two **live devices** after writing the eMMC and patches the SD if
they still match.

Why `deadbee5` and not the obvious `deadbeef`: `flash.sh` runs on the Mac and
can't read the eMMC's signature, so it picks a value safe under every plausible
eMMC state — the eMMC is on `076c4a2a` today, but an earlier session patched it
to `deadbeef` live, and a card flashed now might meet a board in either state.
`deadbee5` differs from both. `emmc-install.sh` compares live-to-live precisely
because that guesswork is avoidable once you're on the board.

Nothing needs patching inside the image: `k3_falcon_fdt_fixup()` derives
`root=PARTUUID` from the live partition table at boot
(`CONFIG_SPL_PARTITION_UUIDS=y`), so the FIT's baked-in cmdline string is
overwritten anyway. An earlier attempt patched that string at a fixed `.wic`
file offset — unnecessary, and it made every rebuild require re-patching.

Doing it by hand, host-side on macOS:
```
diskutil unmountDisk /dev/disk4                          # else "Resource busy"
sudo dd if=/dev/rdisk4 of=/tmp/mbr.bin bs=512 count=1    # read sector 0
printf '\xef\xbe\xad\xde' | dd of=/tmp/mbr.bin bs=1 seek=440 count=4 conv=notrunc
sudo dd if=/tmp/mbr.bin of=/dev/rdisk4 bs=512 count=1    # write the sector back
sync
```
Raw disk devices on macOS require **sector-aligned (512-byte) reads and
writes** — `dd bs=1` directly against `/dev/rdiskN` silently no-ops. Always
read/patch/write a full sector via a local intermediate file. (On Linux, on the
board, `dd bs=1 seek=440` against the block device is fine.)

### Stale ext4 superblock on the eMMC (bad geometry)

The eMMC's p2 can carry a superblock from a previous install whose size doesn't
match the partition table our image writes:

```
EXT4-fs (mmcblk0p2): bad geometry: block count 3794688 exceeds size of device (198767 blocks)
```

These two numbers must agree:
```
grep mmcblk0 /proc/partitions                                     # p2 size in KB
dumpe2fs -h /dev/mmcblk0p2 | grep -i "Block count:\|Block size:"  # count x 4096
```
`emmc-install.sh` compares them automatically and refuses to continue on a
mismatch. Right after a fresh write: 868572 KB vs 217143 × 4096 = 868572 KB.
**These numbers change again on first successful boot** — a growpart/resize2fs
service expands p2 to fill the device and both settle at 15178752 KB /
3794688 × 4096. So `3794688` is the correct steady-state value for a grown
eMMC; it's only a problem when the partition table disagrees.

**Caveat on verifying a `dd` to a block device from a running system.** An
`xzcat f.xz | cmp - /dev/mmcblk0` once reported a byte-perfect match while the
very next boot still read the *old* superblock — most likely because `cmp` read
back through the page cache rather than the physical media (`conv=fsync` flushes
at the end but does not invalidate the cache). Drop caches first; it costs
nothing:
```
sync; echo 3 > /proc/sys/vm/drop_caches
```

### Known-good eMMC boot signature (for diffing)

Diffing a bad boot against a known-good one was by far the most effective
diagnostic technique in this project — much more so than reasoning about
symptoms. Reference for a healthy no-USR eMMC boot with the SD removed:

```
Trying to boot from MMC1
Loading falcon payload from MMC1
Starting ATF on ARM64 core...
[    0.000000] Kernel command line: console=ttyS2,115200n8 root=PARTUUID=<emmc-sig>-02 rootwait
[    2.262286] EXT4-fs (mmcblk0p2): mounted filesystem ... ro with ordered data mode.
beagleplay-ti login:
```
then a login prompt for an instant before the gauge cluster takes the display.

Healthy-boot invariants:
- `MMC1` (not `MMC2`) on both the "Trying to boot" and "Loading falcon payload"
  lines. MMC2 there means the SD-variant SPL ended up in `boot0`.
- Root mounts from `mmcblk0p2`; **no** `recovery required` line.
- `<emmc-sig>` is whatever the eMMC's own MBR disk signature currently is, not
  a fixed value — `k3_falcon_fdt_fixup()` derives it at boot. **Confirmed
  2026-08-09 over SSH: the eMMC is on the image's stock `076c4a2a-02`**, so the
  live `deadbeef` patch an earlier session applied was undone by a later image
  write. **The invariant that matters is that the SD's and the eMMC's differ**,
  not what either one is. Check with `blkid /dev/mmcblk0p2` and
  `blkid /dev/mmcblk1p2`.
- The **only** `[FAILED]` is psplash — cosmetic, present in every successful
  boot on this image.
- `wl18xx` wifi firmware errors are normal and harmless (that firmware isn't
  installed). They appear in every good boot and are *not* a hang, even though
  output often pauses around them.
- With the SD inserted, expect extra `mmcblk1` lines; those are inert. Root
  must still be `mmcblk0p2` — if it's `mmcblk1p2`, the PARTUUIDs collide.
- Nothing about eMMC changes how the app starts: `ultima-app.service` is
  `WantedBy=multi-user.target` and tidss is force-loaded from
  `/etc/modules-load.d/`. If the cluster doesn't come up, look at the image on
  the user area, not at the boot path.

## Always shut down cleanly — `sync; poweroff`

Repeatedly yanking power (especially out of the `### ERROR ### Please RESET the
board ###` state during a failed eMMC boot test) leaves the ext4 rootfs dirty
enough that fsck can no longer auto-repair it. The symptom is **very
misleading** — it looks exactly like a hard hang:

- board alive, heartbeat LED blinking
- display shows a blinking cursor
- keyboard appears to do nothing
- serial goes completely silent partway through boot

It is not hung. It is sitting in systemd **emergency mode** waiting for input
that never arrives:

```
[FAILED] Failed to start File System Check on Root Device.
[  OK  ] Started Emergency Shell.
[  OK  ] Reached target Emergency Mode.
Press Enter for maintenance
(or press Control-D to continue):
```

Diagnose by diffing against a known-good log. The tell is *which* fsck failed:
a failure on an automount is harmless and appears in healthy boots too, but
`File System Check on **Root Device**` is fatal.

This bit us **three times**:

- **SD rootfs**, twice, from power-cycling out of the `### ERROR ###` state
  during failed eMMC boot tests. Fixed by reflashing (12s) — and with `flash.sh`
  now patching the disk signature, that no longer silently reintroduces a
  PARTUUID collision the way a manual reflash used to.
- **eMMC rootfs**, from pulling power to remove the SD card right after the
  first successful eMMC boot. The give-away in the log:
  ```
  EXT4-fs (mmcblk0p2): INFO: recovery required on readonly filesystem
  EXT4-fs (mmcblk0p2): recovery complete
  root: fsck 0.0% complete...  [FAILED] Failed to start File System Check on Root Device.
  EXT4-fs (mmcblk0p2): warning: mounting fs with errors, running e2fsck is recommended
  ```

In one case the kernel had already force-remounted read-write to recover, which
makes `systemd-fsck-root.service` refuse to run at all (it won't fsck an
already-rw-mounted filesystem) and fail outright — that failure is what drops
you into emergency mode. Reflashing is the fast fix there; in-place repair only
works from a system where that filesystem isn't the root.

### Repairing the eMMC rootfs (offline, from an SD boot)

The SD card is the recovery path — keep it bootable. Boot it (hold USR) so the
eMMC is not the root filesystem, then:

```
findmnt -no SOURCE /                       # confirm /dev/mmcblk1p2 (SD), not mmcblk0p2
umount /run/media/rootfs-mmcblk0p2         # release the automounts
umount /run/media/boot-mmcblk0p1
mount | grep -c mmcblk0                    # must be 0
e2fsck -f -y /dev/mmcblk0p2
sync; poweroff
```

**`e2fsck` exit code 1 means "errors corrected" — that is success, not
failure.** 0 = already clean, 1 = fixed, 2 = fixed but reboot needed, ≥4 =
uncorrected errors (only then is it actually bad). Confirm with
`dumpe2fs -h /dev/mmcblk0p2 | grep -i state:` → `clean`.

## Board access over SSH (use this, not serial)

An earlier session recorded "the board has no network (`eth0`/`eth1`/`wlan0`
all `DOWN`)" and concluded serial was the only way in. **That was wrong**, and
it cost a lot of time. A booted board answers on the network with no setup:

```
ssh root@beagleplay-ti.local            # no password
```

What makes it work, all already in the image (verified in the built rootfs):

- `systemd-networkd.service` enabled, with `10-eth.network` / `15-eth.network`
  matching `eth0`/`eth*` at `DHCP=yes` — ethernet configures itself at boot.
- `dropbear.socket` enabled (socket-activated SSH), with `-B` in
  `/etc/default/dropbear` to permit blank-password logins.
- `root::` in `/etc/shadow` — no root password at all.
- `avahi-daemon` enabled, so `beagleplay-ti.local` resolves over mDNS without
  needing to know the DHCP lease.

The earlier "no network" observation was almost certainly a board sitting in
emergency mode, or one with no cable in — not a property of the image.

**Passwordless root over SSH is wide open to anything on the same network.**
That is fine for a bench board on a home LAN and is what makes the tooling
here simple, but don't put this image on a hostile network as-is.

Practical notes:

- **The host key changes whenever a card is reflashed** — `dropbearkey`
  generates a fresh one on first boot, and the SD and eMMC systems both call
  themselves `beagleplay-ti`. Pinning them makes every reflash produce
  `REMOTE HOST IDENTIFICATION HAS CHANGED` and a hard refusal, so the scripts
  pass `-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null`.
- `ssh root@beagleplay-ti.local 'sync; poweroff'` is the clean-shutdown path
  with no console attached. Given how many times unclean power-off has
  corrupted a rootfs on this board, this alone is worth having.
- File transfer uses `ssh 'cat > /path'` rather than `scp`, so it depends on
  nothing but a shell on the far end.
- Serial is still the only way to see anything *before* Linux starts — ROM
  messages, SPL, falcon handoff. For everything after that, SSH is better in
  every respect.

## Serial console caveats (macOS)

Serial is `/dev/cu.usbserial-0001` @ 115200 8N1. Several independent traps
here, each of which produced silent, misleading "nothing is happening"
symptoms:

- **`cat` buffers.** `cat <&3 > logfile` can sit on kilobytes of captured data
  without flushing, so the log looks empty (0 bytes) while the board is
  actually talking. Always use **`cat -u`**. `lsof -p <pid>` showing a nonzero
  offset on the log file while `wc -c` says 0 is the giveaway.
- **Stale readers steal bytes.** A `cat` from a previous capture that was never
  killed keeps the port open and silently consumes input, so new captures come
  up empty with no error. Always
  `for p in $(lsof -t /dev/cu.usbserial-0001); do kill -9 $p; done` before
  arming a new one.
- **TX can fail while RX still works.** At one point every
  `printf ... > /dev/cu.usbserial-0001` did nothing while boot logs kept
  arriving perfectly — a loose TX wire at the header. Reseating fixed it. Test
  with: send `\r`, expect the getty to reprint `beagleplay-ti login:`.
- **Long commands get corrupted.** Beyond ~80-90 characters, expect dropped and
  duplicated characters (`dd` → `ddd`, filenames split by injected CR). Keep
  commands short (~60-70 chars), `clear` before each so the tail is easy to
  read, redirect output to a file and `cat` it back, and prefer **downloading a
  script over HTTP** to typing anything complex — which is exactly what
  `emmc-serve.sh` / `emmc-install.sh` are for.
- **Don't try to parse live echo.** The console's `\r`-only line rewrites
  interleave with kernel messages and make raw output very hard to read
  correctly. Write to a file on the board and `cat` it back. To check a file's
  contents, prefer several short `grep -c <substring>` calls (each individually
  easy to eyeball) over one long pattern or a hash string — long strings are
  exactly what gets corrupted in transit.
- **The CP2102 adapter itself can wedge.** Symptom: the device node exists and
  `ioreg` still lists the adapter, but `stty` fails with
  `tcsetattr: Invalid argument` and the port stays stuck reporting
  `speed 9600 baud`. Unplugging and replugging the **USB end** clears it. Worth
  checking whenever a capture goes silent — it looks identical to a dead board.

### Transferring files to the board

Don't type them. Bring up ethernet and pull over HTTP — `./emmc-serve.sh` on
the Mac does the staging and prints the exact board-side line. There is no
WiFi in this build; wired ethernet is the only network path.

### Shell gotcha worth remembering: `pipefail` + `grep -q`

`strings f | grep -q PATTERN` inside a `set -o pipefail` script reports
**failure on a successful match**: `grep -q` exits at the first hit, `strings`
then dies of SIGPIPE (141), and `pipefail` surfaces that as the pipeline's
status. This produced a completely misleading "not the eMMC variant" refusal on
a file that plainly was. Capture instead — plain `grep` drains its input:

```sh
V=$(strings "$f" | grep -E '^mmcdev=' || true)
[ "$V" = "mmcdev=0" ] || die ...
```

### Serial automation lessons (for next time this board needs live debugging)

- `screen -X stuff` (remote command injection into a daemonized `screen`
  session) **does not work** in this assistant's execution environment —
  confirmed via an isolated test unrelated to the board. Daemon-mode
  (`-dmS`) `screen` is fine for passive logging (`-L`) but can't be driven
  interactively this way.
- `expect`'s `spawn screen <device> <baud>` (a direct foreground spawn, not
  daemon+reattach+`-X stuff`) **does** work for interactive read/write.
- Empirically, a fresh `expect` spawn sometimes captures zero bytes even
  though the board is fine and the port isn't held by anything else
  (checked via `lsof` and `ioreg`) — a physical USB-serial adapter replug
  immediately before the `expect` attempt reliably fixed this, though the
  underlying cause was never root-caused. Not every dead-looking attempt is
  this, though: once, "no board response" turned out to be the board
  genuinely idle at a `sulogin` maintenance prompt that doesn't reprint
  itself on a bare `\r` — always check for real evidence (a raw `cat`/`dd`
  read with zero bytes, not just an expect-pattern miss) before assuming
  it's a connection problem.
- A `screen -ls`/`quit` sweep plus an `lsof` check on the device node before
  every new attempt avoids "Resource busy" from a previous attempt's
  half-dead session still holding the port.
- A new failure mode (2026-08-09, not seen earlier in this project): the
  daemon `screen` session itself vanished outright — `screen -ls` went from
  showing it `(Attached)` to "No Sockets found" with no `quit`/kill issued —
  multiple times in one sitting, each time needing a fresh `-dmS` session
  under a new name. Not root-caused (macOS terminal/pty churn? adapter
  hiccup that didn't drop the `/dev` node itself?). Before concluding "no
  data = broken connection": a raw `cat`/`dd` read showing zero bytes only
  rules out screen-level issues — it does NOT distinguish "board genuinely
  silent because it's idle and nothing's been sent to it" from "connection
  actually broken." Send an actual keystroke (not just passive read) before
  drawing conclusions, and ask what the heartbeat LED is doing — that's the
  fastest ground-truth check for "is it even powered/running" independent
  of serial entirely.
