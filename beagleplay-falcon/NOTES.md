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

**Update (2026-08-11): power-on → first Qt frame measured directly, not
proxied.** The ~6.94s number above was actually `ultima-app.service`
starting, not a frame on screen. `ultima-app` now hooks
`QQuickWindow::afterRendering`/`frameSwapped` and logs the real first paint
to `/dev/kmsg` at level 3, which survives the `quiet` cmdline and lands on
the serial console in the same timeline as power-on — no cross-clock
offset math needed. Two hardware runs: **6.664s and 7.298s**. See
"Boot-time measurement: first Qt frame" below.

**Update (2026-08-11): further-optimized round, ~5.4s → ~4.5s kernel-clock
to first frame on eMMC** (`ultima-app.service` decoupled from
`multi-user.target`'s wait on `systemd-resolved`/D-Bus, plus a boost-ring
`Canvas` size fix and a wasted-decode fix). See "Boot-time investigation"
and "Boot-time optimization, round 2" below for the full investigation,
a real bug found and fixed on hardware (not caught by the macOS dev build),
and an odometer-persistence correctness scare that turned out to be a test
methodology bug, not a real one — worth reading before trusting the
`ultima-app.service` unit file's `Wants=`/`Requires=` choice again.

**Confirmed with a real serial capture (2026-08-11, same day, eMMC,
`measure-boot.sh`): power-on → first Qt frame is now 5.238s** — down from
6.664s/7.298s (the two hardware runs in "Boot-time measurement: first Qt
frame" below), a genuine **1.4–2.1s faster**, real power-on `t=0`, not
proxied or estimated from kernel-clock deltas. See "Round-2 serial
verification" further down for the raw capture.

**Update (2026-08-11): round 3 — another ~1.1s found, fixed, and confirmed
with a real serial capture, same day.** `ultima-data-mount.service` and
`ultima-app.service` both carried a stale `After=systemd-udevd.service`, the
same class of bug as the already-fixed `fb0`/tidss holdover — the block
device they actually need comes from devtmpfs at kernel driver-probe time
(~0.48s), over a second before udevd finishes starting (~2.7s). Proven with
a live hand-edit, ported to source, rebuilt, reflashed to SD then eMMC
(first frame kernel_ts 4.470s → 3.338s on eMMC), odometer-persistence
re-verified clean twice along the way. **Real serial power-on capture:
4.536s**, down from round 2's confirmed 5.238s and **2.13–2.76s faster than
the original 6.664s/7.298s baseline**. See "Boot-time optimization, round
3" below for the full investigation, including a `build.sh`
stale-image-pull trap hit and fixed along the way.

**Update (2026-08-11): investigated a pre-cluster boot splash — the kernel's
built-in fbcon boot logo doesn't work on this DRM/tidss path, confirmed
empirically on hardware, not just in config.** All the kernel config it needs
(`CONFIG_FRAMEBUFFER_CONSOLE`/`CONFIG_LOGO`/`CONFIG_LOGO_LINUX_CLUT224=y`,
deferred-takeover off) is already enabled — but a raw `/dev/fb0` dump at rest
was **all zero bytes**, and writing text to `/dev/tty1` only ever produced a
few hundred nonzero bytes (the text glyphs), never the logo. Also found and
fixed a real bug along the way: `getty@tty1` was overwriting the console with
a login prompt before Qt's first frame. See "Boot splash investigation"
further down for the full evidence and the panel facts (1600×720, stride
6400, 32bpp, 4 online CPUs) worth keeping for whatever replaces this — likely
a small userspace program that blits directly to `/dev/fb0`, which this
investigation confirmed is writable and gets a real ~2.5s window
(fbcon binds at kernel_ts 0.77s; first Qt frame at 3.34s).

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
   was `CONFIG_DRM=m` capping everything downstream via ordinary tristate
   dependency capping — *not* a Kconfig `choice` group, see correction below.
   Fix: explicitly force `CONFIG_DRM=y`,
   `CONFIG_DRM_KMS_HELPER=y`, `CONFIG_DRM_TIDSS=y`, `CONFIG_DRM_ITE_IT66121=y`,
   `CONFIG_DRM_DISPLAY_HELPER=y`, `CONFIG_DRM_DISPLAY_CONNECTOR=y` in
   `ultima-display.cfg` (that fragment also left `# CONFIG_DRM_POWERVR is not
   set`, but — see the correction below — that line was never load-bearing
   for this fix). Verified against the built `.config`. This also let
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

**Correction (2026-08-12):** point 1 above originally called
`CONFIG_DRM_POWERVR` "the competing choice member" against `CONFIG_DRM_TIDSS`.
That's wrong — read directly against this kernel's source, there are zero
Kconfig `choice` blocks anywhere under `drivers/gpu/`, and upstream's own
arm64 defconfig sets `CONFIG_DRM_TIDSS=m` and `CONFIG_DRM_POWERVR=m`
*together*. They're independent tristates; `CONFIG_DRM=m` capping
`CONFIG_DRM_TIDSS` was the entire mechanism, and disabling PowerVR never had
anything to do with fixing it. See "PowerVR GPU enablement" further down for
the full investigation — PowerVR is no longer left disabled for this reason.

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

## Boot-time measurement: first Qt frame (2026-08-11) — done, hardware-measured

The "Gauge cluster actually rendering" row above (~6.94s) was never actually
a frame — it was `journalctl`/`dmesg`-derived, and the closest thing to a
render signal available at the time was `ultima-app.service` starting.
Between process start and an actual QML frame hitting the framebuffer there's
Qt/QML engine init, scene graph setup, and the first paint — unmeasured. This
closes that gap with a real render-completion signal.

**Instrumentation** (`ultima-app/main.cpp`): `main()` already logged
`t0`-`t3` timestamps (via `/proc/uptime`, same clock family as kernel
monotonic time — no wall-clock conversion needed) up through QML component
construction, but that happens *before* `app.exec()` even starts the event
loop, so it's construction, not a frame. Added a one-shot hook on the root
`QQuickWindow`'s `afterRendering()` and `frameSwapped()` signals
(`Qt::DirectConnection`, guarded by `std::atomic<bool>` so it's safe
regardless of which thread the software render loop fires them on — cheap
insurance, not confirmed to matter here). Logs both to stderr (journal, same
as the existing `t0`-`t3` lines) and to `/dev/kmsg` at level 3.

**Why `/dev/kmsg` and not just the journal**: the "Boot-time optimization"
`quiet` patch above sets `console_loglevel=4`, and printk only suppresses a
message if its level is `>=` that — a level-3 write still reaches the serial
console. Verified empirically before touching any code, over SSH with a
serial capture watching: `echo '<3>ultima-kmsg-test: hello world' > /dev/kmsg`
landed on console as `[23010.325822] ultima-kmsg-test: hello world`. This
means power-on through first-frame now lives in **one serial capture, one
timeline, no offset arithmetic** — a real improvement over the "Boot-time
optimization" run above, which had to reconstruct a wall-time offset from two
separate anchor points because its landmarks straddled a `quiet`/non-`quiet`
boundary.

`measure-boot.sh` updated to match: new `first Qt frame` landmark
(`ultima-app: \[.*\] first frame rendered`), now the stop landmark instead of
the dead `framebuffer ready` regex. `kernel entry`/`framebuffer ready` are
expected to read "not seen" on this image — annotated in the script so that
doesn't look like a failure to whoever runs it next.

### New serial gotcha: stale UART backlog contaminates `t=0`

Cost two wasted captures before being caught. The tty driver buffers bytes
that arrive with no reader attached (up to its buffer size), and they flush
out as soon as something opens the port — even if that data is from a boot
that finished, or partly happened, before the capture started. `measure-boot.sh`
treats the *first byte it reads* as `t=0` (power-on proxy), so a leftover
byte from a previous shutdown or boot silently produces a completely wrong
zero point, and everything downstream looks plausible (increasing
timestamps, real-looking landmark text) right up until the numbers don't
line up with a previous run. Existing "kill stale readers" guard doesn't
catch this — the port isn't held by anything, the bytes are just sitting in
the driver's own buffer.

Symptom in practice: one capture's very first line was
`[  196.691147] reboot: Power down` (a stale byte from an unrelated earlier
shutdown), and the real boot didn't start until 13.4s of wall-clock time
*later* — the script's own landmark table was silently 13.4s off for that
run. Recovered by hand by finding the true power-on line (`U-Boot SPL
2025.01...`, the first line that's actually part of a real boot) and
re-deriving every landmark relative to that instead of the script's `t=0` —
usable, but shouldn't be necessary.

**Fix for next time**: drain the port to 2s of confirmed silence
*immediately* before arming a capture, not just at session start:
```
exec 3<>/dev/cu.usbserial-0001
stty -f /dev/fd/3 115200 cs8 -cstopb -parenb raw -echo
python3 -c "
import os, select
while True:
    r, _, _ = select.select([3], [], [], 2.0)
    if not r: break
    os.read(3, 65536)
"
exec 3<&-
```
Confirm the board is actually powered off before doing this (otherwise
"quiet" just means nothing's printing, not that a fresh capture is safe).
Not yet folded into `measure-boot.sh` itself — done by hand for this
session's runs, worth adding as an automatic pre-arm step if this bites
again.

### Results: two hardware runs, real power-on to first Qt frame

| Landmark | Run 1 (reconstructed, see above) | Run 2 (clean) |
|---|---|---|
| falcon payload load | 0.170s | 0.307s |
| ATF start | 0.939s | 1.077s |
| **first Qt frame** (afterRendering) | **7.298s** | **6.664s** |
| first Qt frame (frameSwapped) | 7.307s | 6.675s |

Both are real, physical-power-cycle boots, no USR held (eMMC, same image
this whole file already describes). Run 2 is the clean capture — `t=0`
correctly anchored, `measure-boot.sh`'s own landmark table matches the
by-hand numbers exactly. Run 1's numbers are hand-derived from a
contaminated `t=0` (see gotcha above); kept because the underlying boot was
real and complete, not because it's as trustworthy as run 2.

Note the two runs disagree on which stage is slower: run 2's bootloader
stage (payload-load → ATF-start) is ~0.14s slower than run 1's, but its
kernel-entry-to-first-frame stretch is ~0.77s *faster* (5.587s vs 6.359s),
netting out faster overall. This is consistent with "Boot-time optimization"
already flagging the post-bootloader/pre-app stretch as the least
deterministic remaining phase (systemd's path to starting
`ultima-app.service`) — not a new finding, just corroborating it with a
tighter instrument. **Power-on → first Qt frame currently lands in the
6.7–7.3s range**; a third run was judged not worth another physical
power-cycle for this session, but would be the natural next step to tighten
this the way the original 3-run baseline table did.

## Boot-time investigation: where the remaining ~6.7s goes (2026-08-11)

Static analysis (source + kernel config + unit files + existing boot logs),
no hardware touched this pass. Goal was to find where to spend further
optimization effort — corrects an apportionment mistake made mid-investigation,
worth recording so it isn't repeated.

### Apportionment: pre-app vs. app-side — do this on the kernel clock, not wall clock

Run 2's log (the clean 6.664s capture) gives a rock-solid wall↔kernel offset,
agreeing to the millisecond across all four landmarks that appear on both
scales: **wall = kernel_ts + 1.245s**. First Qt frame is at kernel_ts
**5.419s**.

The "Boot-time optimization" section above (2026-08-10) measured, via
journalctl on that day's boot, that `ultima-app.service` itself starts at
**kernel_ts ≈ 4.0s** (the "systemd's own path to starting ultima-app.service"
finding, kernel_ts 0.8s→4.0s). That number comes from a different boot
session than run 2 (a day earlier, before the kmsg first-frame instrumentation
existed) — the kernel/rootfs are the same image family, but this is still an
approximation, not two landmarks from one capture. Treat it as directionally
right, not precise.

Combining them properly (both on kernel_ts, not mixing one kernel-relative
number with one wall-clock number — an error made and caught mid-investigation
here): **service start ~4.0s, first frame at 5.419s → app-side (process exec
to first frame) is ~1.4s, and everything before the service even starts
(kernel driver probe + systemd's sysinit→basic→multi-user chain) is ~4–5.2s
of the ~6.7s wall total.**

That's roughly a 3:1 split in favor of the pre-app path. **This inverts the
naive read of "quiet boot means we can't see where kernel-to-app time goes,
so guess from the app side"** — the bigger lever is on the systemd/kernel
side, not in `ultima-app`'s own QML/image startup, even though the latter is
far easier to reason about from source alone.

### Measured directly on hardware (2026-08-11), board already up — no power cycle

`systemd-analyze` isn't installed on this image (`sh: systemd-analyze: command
not found` — this is a trimmed Arago image, not a full distro), so the
apportionment came from `journalctl -b -o short-monotonic --no-pager`
(unit-transition timestamps, all on the kernel monotonic clock — no offset
math needed) plus `journalctl -b -o short-monotonic -u ultima-app` for the
app's own `t0`–`t3`. Both are one SSH command against an already-booted
board, no serial capture, no reflash. Full captures saved (gitignored, like
the serial `boot-logs/` above) to `boot-logs/journalctl-timeline-20260811T095X.txt`
and `boot-logs/journalctl-ultima-app-20260811T095X.txt`; key transitions:

| kernel_ts | Event |
|---|---|
| 0.976 | root (`mmcblk0p2`) mounted ro, devtmpfs mounted |
| 2.741 | `systemd-udevd` started |
| 2.778–2.888 | `ultima-data-mount.service` runs, `/data` (`mmcblk0p3`) mounted rw |
| 2.919 | **Local File Systems target reached** |
| 3.170→3.627 | `systemd-resolved` starting→started (**0.457s**, not needed by this app) |
| 3.629 | System Initialization target (`sysinit.target`) |
| 3.685→3.915 | D-Bus system bus starting→started (**0.230s**, not needed by this app) |
| 3.932 | **Basic System target reached (`basic.target`)** |
| 4.046 | `ultima-app.service` starts |
| 4.505 | app's own `t0` (`main()` entered) — **0.46s after systemd starts the unit**, this is process exec + dynamic-linking Qt/QML shared libs |
| 4.632 | `t1`: `QGuiApplication` created (**+0.16s** — platform-plugin init, fast) |
| 5.355 | `t2`: QML loaded (**+0.72s** — the dominant app-side cost, see Candidates 2/3 below) |
| 5.356 | `t3`: ready to render (+0.00s) |
| 5.418 | **first frame rendered** (+0.06s past `t3`) |
| 6.215 | Multi-User System target reached (well *after* first frame — sibling units like getty, network config, `/data`+boot-partition fsck all run concurrently with, not ahead of, the app) |
| 6.447 | systemd's own "Startup finished" line: 1.068s kernel + 5.378s userspace |

Two things settled precisely, replacing the earlier estimates:

1. **App-side (`t0`→first frame) is 0.94s**, split 0.16s Qt init / 0.72s QML
   load / 0.06s first paint — confirms the QML-load step (image decode +
   first `Canvas` paint) is where nearly all of it goes, exactly what
   Candidates 2 and 3 below target.
2. **`ultima-app.service` doesn't start until 4.046s, but its real
   prerequisites (`systemd-udevd` + `/data` mounted) are done by 2.919s.**
   The ~1.1s in between is `basic.target`'s other dependents —
   `systemd-resolved` (0.457s) and D-Bus (0.230s) are the two biggest single
   pieces — none of which this app uses. **This is a real, now-measured
   ~1.0–1.1s available, not a hypothetical.**

Also checked while here: `/data/odometer.json` currently holds
`{"totalOdo":2347,"tripOdo":0}` — the hardcoded default, being re-saved every
30s. **Not a bug** — confirmed via `findmnt`/`stat`: `/data` is mounted
correctly (rw, `mmcblk0p3`) and the file is genuinely fresh (this is a bench
board with no real drive history yet), not a symptom of a mount race. Also
confirms current margin under the *existing* config: the mount completes at
2.888s and `main()` doesn't run until 4.505s — over 1.6s of slack today, which
is exactly the slack Candidate 1 below would spend.

### Candidate 1 (biggest, ~1.0–1.1s, now measured — but still a real correctness risk): stop `ultima-app.service` waiting on `basic.target`

`ultima-app.service` is `WantedBy=multi-user.target` with default systemd
dependencies, which implicitly adds `After=basic.target` — confirmed above,
it waits for `systemd-resolved` and D-Bus to fully start even though it uses
neither:

- `/dev/fb0` doesn't need waiting for — `tidss` is fully kernel-built-in
  (`CONFIG_DRM_TIDSS=y` etc., see "Boot-time optimization" above), so the
  device node exists via devtmpfs before systemd (PID 1) even execs, not via
  a udev coldplug/module-load event the way it used to. The `After=
  systemd-udevd.service`/`Wants=systemd-udevd.service` in the unit today is
  very likely a holdover from when tidss was still a module.
- CAN doesn't need waiting for — `CanBus::tryConnect()` already retries every
  1s if the interface isn't present (`canbus.cpp`), by design, specifically
  so app start never races udev (see `GAUGE-CLUSTER.md`).
- `/data` (odometer persistence) genuinely does matter, and is already
  ordered correctly today — but only *incidentally*: `ultima-data-mount.service`
  carries `Before=local-fs.target ultima-app.service`, and both units
  currently end up in the same boot transaction because both are reachable
  from `multi-user.target`. That ordering guarantee would break if
  `ultima-app.service` stopped being pulled in through that same transaction.

`ultima-data-mount.service` already proves the pattern works on this hardware
(`DefaultDependencies=no` + explicit minimal `After=`, see its own `[Unit]`
comment) — mirroring it on `ultima-app.service` (rough shape:
`DefaultDependencies=no`, `WantedBy=sysinit.target` or similar, drop the
`multi-user.target` wait) is the obvious next move. **Two traps, both silent
until they bite:**

1. **`Before=`/`After=` is ordering only, not a dependency that pulls a unit
   into the transaction.** If `ultima-app.service` is reachable from
   `sysinit.target` independently of `local-fs.target`, systemd can start it
   without `ultima-data-mount.service` ever running first — the existing
   `Before=` on the data-mount unit has nothing to order against in that
   transaction. Silent failure mode: `OdoStore::load()` falls back to
   `DEFAULT_TOTAL_ODO = 2347.0`, and the 30s autosave `Timer` in `main.qml`
   writes that default over the real `/data/odometer.json` — a quiet
   odometer reset that looks like random data corruption, not a boot-order
   bug. (This exact symptom — `totalOdo=2347.0` on every save — is what's
   currently on the bench board for the unrelated, benign reason above; a
   real instance of this bug would look identical, which is exactly why it's
   dangerous.) Fix: add an explicit `Requires=ultima-data-mount.service` +
   `After=ultima-data-mount.service` on `ultima-app.service` itself, don't
   rely on the other unit's `Before=` alone. (Unlike the `dev-fb0.device`
   trap this file already warns about, `After=` on a `Type=oneshot` +
   `RemainAfterExit=yes` unit genuinely does wait for it to finish — that
   scar tissue doesn't apply here.)
2. **`DefaultDependencies=no` also drops the automatic
   `Conflicts=shutdown.target`/`Before=shutdown.target`** that currently
   makes `ultima-app`'s `SIGTERM` handler (which saves the odometer, see
   `sigHandler` in `main.cpp`) run before `/data` gets unmounted on shutdown.
   Needs re-adding explicitly, the same way `volatile-binds`' own service
   template does.

Given this file's two prior hardware-discovered ordering bugs in this exact
area, implement this carefully and re-run the same `journalctl` timeline
after, checking specifically: `ultima-app.service`'s start time moved earlier
by roughly the expected amount, `/data/odometer.json` still shows real
(non-reset) content after a save cycle, and a clean `ssh ... reboot` still
persists an odometer write made just before it.

### Candidate 2 (small, safe, on the direct first-frame critical path): shrink the boost-ring `Canvas`

`main.qml`'s `boostRing` `Canvas` is `anchors.fill: parent` — a full
1600×720 backing store allocated and painted under `QT_QUICK_BACKEND=software`
(CPU rendering, no GPU), just to draw a pie-wedge mask around the tachometer
(center 1251,343, radius `max(width,height)*1.5`). It paints
unconditionally before first frame (`Component.onCompleted: loadImage(...)`
→ `onImageLoaded: requestPaint()`). Sizing the `Canvas` down to just the
tach's actual bounding box (~600×600 around 1251,343, matching `rpmGauge`'s
own footprint) instead of the full window would cut both the backing-store
allocation and the per-paint cost substantially — needs reworking the
`centerX`/`centerY`/clip-arc math in `onPaint` to the smaller local coordinate
space, not just a resize.

### Candidate 3 (small, safe): collapse `car_lights_on.png`/`car_lights_off.png`

Both are separate full-frame (1600×720 RGBA, ~4.6MB decoded) `Image`
elements in `main.qml`, gated only by `visible:` — QtQuick decodes a local
(qrc) `Image`'s source synchronously at load time regardless of `visible`,
so both get decoded every boot even though only one is ever shown. Rough
estimate ~50–150ms wasted. Two ways to fix, different tradeoffs:
- One `Image` with `source` switched dynamically between the two — simplest,
  but re-decodes on every on/off toggle (not just at boot).
- Keep both elements, but mark whichever isn't the boot-default state
  `asynchronous: true` (or move it behind a `Loader` activated after first
  frame) — keeps instant toggling, moves the wasted decode off the critical
  path instead of removing it.

### Candidate 4 — tried, reverted: `CONFIG += qtquickcompiler` is actively broken on this Yocto build, not just low-value

The reasoning for trying this was sound (root read-only + tmpfs `/tmp` means
Qt's runtime QML disk cache can never persist here, so every boot pays full
QML parse/compile cost with no way to amortize it) and it works cleanly on
the macOS Qt6 Homebrew dev build (`scripts/dev-build.sh` — real
`qmlcachegen` invocations per QML file, confirmed in the build log). **It is
not safe on the Yocto/Qt5 build as this layer is configured today.**

Adding `CONFIG += qtquickcompiler` to `ultima-app.pro` and rebuilding hung
`ultima-app`'s `do_compile` indefinitely — not a clean failure, an infinite
loop. Root cause, found by reading the live compile log (which had grown to
36MB and was still climbing before being killed):
`recipe-sysroot-native/usr/bin/qmlcachegen` **does not exist** in this
build's native sysroot — meta-qt5's `qtdeclarative-native` recipe here isn't
configured to produce it. Rather than qmake failing cleanly when the tool is
missing, it re-invokes `qmake -o Makefile ...` in a loop that never
converges, burning CPU indefinitely until something kills it. Confirmed by
`ps`/`docker ps` there was no actual progress, just the same block repeating
in the log.

Reverted (`ultima-app.pro` back to no `qtquickcompiler`, see its own comment
for the full story). Making this work for real means adding a
`PACKAGECONFIG` to meta-qt5's `qtdeclarative` recipe to build `qmlcachegen`
for `-native` — a real Yocto layer change with its own risk, not attempted
here given the win was already estimated as tens of milliseconds, not
seconds, before this cost was even known. **If this is revisited, confirm
`qmlcachegen` actually lands in `recipe-sysroot-native/usr/bin/` before
flipping the `CONFIG` line again** — don't re-trigger the same hang.

### Candidate 5 (small, image-size lever, not critical-path): drop `wlcore`/`wl18xx`

**Superseded, not acted on — see "WiFi AP" and "WiFi AP + captive portal
abandoned, reverted to client mode" below.** The opposite happened: WiFi was
enabled (2026-08-18), not trimmed. This section is kept for the boot-time
reasoning (still valid background for anyone who *does* want to trim it
later), not as a statement of current build config.

There was no WiFi hardware wired into this build at the time this was
written — `wlcore`/`wl18xx` firmware-load failures appeared in every boot
log, always *after* first frame in the clean run (kernel_ts 6.36s vs. first
frame at 5.42s), so this was **not** a critical-path fix. It's a `fitImage`
size lever: the R5 SPL reads `tifalcon.bin` + `fitImage` off eMMC via its own
ext4 driver before ATF even starts (falcon payload load → ATF start is
~0.77s in run 2), and that read time scales with image size. Trimming
`wlcore`/`wl18xx` out of the kernel/image entirely would shrink `fitImage`
and remove genuinely pointless boot-time work (three firmware retries per
boot for hardware that isn't there), just don't oversell it as a first-frame
win.

### Not investigated further this pass

- Font enumeration: **partially resolved by the real data above** —
  `t0`→`t1` (`QGuiApplication` construction) measured at only +0.16s, so
  whatever the `QFontDatabase: Cannot find font directory /lib/fonts` warning
  in the journal costs, it isn't a dominant chunk on its own. That warning
  actually logs *during* the `t1`→`t2` QML-load window (kernel_ts 5.008, between
  `t1`=4.632 and `t2`=5.355), triggered by the two `FontLoader`s in `main.qml`
  — bounded inside the already-identified 0.72s QML-load cost, not a separate
  lever. Not chased further given Candidates 2/3 already target that window
  directly.
- `SetTimeScreen.qml` is constructed eagerly (`visible: false`, no `Loader`)
  as part of `main.qml`'s component tree — deferring it behind
  `Loader { active: false }` would shave some component-construction cost,
  but likely small next to the Canvas/image items above (0.72s QML-load total
  covers parsing + image decode + this construction + the first `Canvas`
  paint combined — no per-item breakdown finer than that was captured this
  pass). Worth revisiting only after Candidates 2/3 are in and the window is
  re-measured.

## Boot-time optimization, round 2: implemented and hardware-verified (2026-08-11)

Candidates 1, 2, 3, and 5 from the investigation above were implemented and
tested on real hardware (SD card, USR held). Candidate 4
(`qtquickcompiler`) was tried and reverted — see its own section above,
turned out to actively hang the build, not just be low-value.

**Changes:**
- `ultima-app.service`: `DefaultDependencies=no` +
  `Requires=`/`After=ultima-data-mount.service` +
  `WantedBy=local-fs.target` (Candidate 1) — see the unit file's own comment
  for the full reasoning.
- `main.qml`: boost-ring `Canvas` shrunk from full-screen (1600x720) to a
  bounded 640x640 box around the tach (Candidate 2), and
  `car_lights_on.png` marked `asynchronous: true` (Candidate 3).
- `ultima-no-wifi.cfg`: `CONFIG_WLCORE`/`CONFIG_WL18XX`/`CONFIG_WLCORE_SDIO`
  disabled (Candidate 5).

### Bug found on first hardware boot: Candidate 2's Canvas resize crashed drawImage

The macOS Qt6 dev build (default GPU-accelerated Quick backend, not
`QT_QUICK_BACKEND=software`) rendered the resized boost ring with no visible
clipping, which was taken as verification — **wrong**. First real hardware
boot logged `qrc:/main.qml:70: Error: drawImage(), index size error`
immediately after first frame. Root cause: the Canvas's `x`/`y` were
computed as `1251 - width/2` / `343 - height/2` with `width: height: 720`,
and the tach center (343) is only 343px from the window's top edge (window
is 1600x720) — `343 - 720/2 = -17`, a negative `y`. The 9-arg `drawImage`
call's source rect then extended above the source image's actual bounds,
which Qt5's software-backend `drawImage` implementation throws on (Qt6's
default GPU path apparently clamps instead — **don't trust a dev-build
verification that isn't running the same `QT_QUICK_BACKEND`/renderer as the
target**, this was the direct cause of missing it). Fixed by computing the
actual safe bounding box (`2*min(cx, W-cx)` × `2*min(cy, H-cy)` = 698×686 for
this window/center) and using 640×640, comfortably inside it. Verified fixed
via the hot-deploy loop (see below) before the next full image build.

### Fast-iteration hot-deploy loop, exercised for real this session

Used `NOTES.md`'s existing documented loop (build just `ultima-app`, `scp`
the binary to `/tmp`, remount root rw, `mv` into place, remount ro, restart
the service) to test the Canvas fix without a full image rebuild+reflash.
Two new gotchas found, both about `mount -o remount,rw /` specifically (not
previously documented for the *rw* direction, only *ro*):

- It intermittently failed with the same `mount point is busy` this file's
  "Two regressions..." section already documents for `remount,ro` — but
  seen here on `remount,rw` instead, both directions of the same transient
  issue. Retrying, or just checking `findmnt -no OPTIONS /` first (it may
  already be rw from a prior attempt) and skipping the redundant remount,
  both worked.
- A file staged at `/tmp/ultima-app.new` disappeared between one `ssh` call
  confirming its presence and a subsequent one trying to `mv` it — not yet
  root-caused (no matching `tmpfiles.d` rule, `systemd-tmpfiles-clean.timer`
  wasn't due for 8 more minutes), but reliably worked when the `scp` and the
  `mv` happened back-to-back with no other commands (including diagnostic
  ones) in between. Treat the upload and the move as needing to be adjacent,
  not just "eventually consistent."

### Odometer-persistence correctness test: real bug, then a chase, then confirmed clean

This is the exact risk flagged when Candidate 1 was designed (an incomplete
`ultima-app.service` ordering change could make `OdoStore` silently load its
hardcoded default instead of the real persisted value) — worth recording the
full test methodology, since the first three attempts gave a false positive
for the bug being real.

**The trap**: writing a distinctive test value to `/data/odometer.json`
*while `ultima-app.service` is still running*, then rebooting, doesn't test
what it looks like it tests. `main.cpp`'s `sigHandler` — the SIGTERM handler
that's the entire point of `Conflicts=`/`Before=shutdown.target` in the
Candidate 1 change — calls `CanBus::save()`, which pushes the app's own
**in-memory** odometer value (whatever it loaded at *this* boot's own
startup, stale by design if nothing drove real CAN data) back over the file
during systemd's shutdown sequence, which happens *before* `/data` unmounts
— correctly, that's what the fix was for. So a manually-edited value written
while the service is live gets overwritten by the app's own legitimate
shutdown save, every time, regardless of whether the boot-time mount
ordering has any bug at all. Three reboot attempts in a row showed the test
value reverted to the hardcoded default (`2347.0`/`0.0`) and looked exactly
like the ordering bug this test was designed to catch.

**Correct methodology**: `systemctl stop ultima-app` (itself sends SIGTERM
and triggers the same save — so the *value on disk after the stop
completes* is not yet the test value), *then* write the distinctive value,
*then* either `systemctl start` (no reboot, tests `OdoStore::load()` in
isolation) or reboot with the service left stopped (nothing left running to
overwrite the file before shutdown, so the value survives untouched into
the next boot's fresh `ultima-app` start).

**Results, both clean:**
- Live restart (service stopped → write `7777.7`/`3.3` → `systemctl start`):
  first autosave (30s later) logged `saved totalOdo=7777.7 tripOdo=3.3` —
  `OdoStore::load()` itself works correctly.
- Full reboot (service stopped → write `4242.4`/`6.6` → `reboot`, USR held):
  next boot's first autosave logged `saved totalOdo=4242.4 tripOdo=6.6` —
  **the `/data`-before-`ultima-app` boot ordering is confirmed correct
  across a real physical reboot.** `/data` mount consistently finishes
  (`Finished Mount /data...`) at kernel_ts ~2.8–2.99s across every boot
  captured this session, `main()` consistently enters at kernel_ts
  ~3.4–3.6s — comfortable, repeatable margin, not a close call.

Also confirmed clean each boot: zero `systemctl --failed` units, zero
`journalctl -b | grep -i "ordering cycle"` hits, `/var/volatile/tmp` and
`/var/volatile/log` both present (the read-only-rootfs checklist further up
this file) — no regression in any of that from the `DefaultDependencies=no`
change.

### `Requires=` caught before it shipped: would have taken the whole cluster down on a `/data` mount failure

Before the eMMC push, `ultima-app.service` had `Requires=ultima-data-mount.service`
(not just `After=`). Caught in review, not on hardware: `Requires=` means a
*failed* dependency stops this unit from starting at all — not just running
with a wrong odometer value. `/data` is the one partition still mounted
read-write (see "Read-only rootfs" above), and this board loses power with
zero warning in the car — a torn `/data` mount on the next boot is a real
scenario, not hypothetical. Under `Requires=`, that scenario would have
produced **no gauge cluster at all**, directly contradicting this project's
own design goal that a torn `/data` write "can't take the whole board down
with it" (same section). Changed to `Wants=` — still pulls
`ultima-data-mount.service` into the same boot transaction (so the `After=`
ordering has something to resolve against, which was the entire point of
adding it), but a mount failure now degrades to "cluster starts, `OdoStore`
falls back to its default, autosave fails" instead of no dash. Rebuilt,
reflashed, reverified clean (zero `systemctl --failed`, zero ordering-cycle
hits) before pushing to eMMC.

### Final eMMC comparison, same board, same methodology as the original baseline

Pushed the corrected image to eMMC via `emmc-push.sh` — the first fully
end-to-end run of that script (previously only dry-run/non-destructive
checks had exercised `emmc-install.sh`, per "eMMC boot" above). Ran clean:
disk signatures distinct (`emmc=2a4a6c07` vs `sd=e5beadde`), partition/
superblock sizes matched, install log had no errors. Verified after
power-cycling with no button held: `findmnt -no SOURCE /` → `/dev/mmcblk0p2`
(eMMC), zero failed units, zero ordering-cycle hits.

| | Before (2026-08-11 AM, eMMC) | After (2026-08-11 PM, eMMC) |
|---|---|---|
| `ultima-app.service` starts | kernel_ts 4.046s | kernel_ts 2.943s |
| `t0`→`t1` (Qt init) | — | +0.24s |
| `t1`→`t2` (QML load) | +0.72s | +0.87s |
| first frame (afterRendering) | kernel_ts 5.419s | kernel_ts 4.470s |

**Service start ~1.10s earlier, first frame ~0.95s earlier overall** —
confirmed on the same board, same storage device, single before/after
sample each side (this project's own "Boot-time measurement" section notes
this phase isn't fully deterministic run to run, so treat both single
samples as indicative, not final-decimal precise).

**Be honest about what actually moved**: the ~1.10s service-start
improvement cleanly matches Candidate 1's prediction and was reproduced
across 6+ boots this session (SD and eMMC combined) — that part is solid.
**QML-load time did not improve** (0.87s after vs. 0.72s before, and this
session's SD-boot runs alone ranged 0.72–0.97s) — Candidates 2/3 (the
`Canvas` resize, the deferred image decode) **did not demonstrably help in
this single comparison**, and might even be a slight net regression (the
9-arg cropped `drawImage` call could plausibly be more expensive per-pixel
than the old 4-arg full-scale draw it replaced — not measured directly).
Don't fold that into the systemd win. If this matters enough to chase
further: multiple eMMC runs to establish real variance, and/or instrumenting
around just the `Canvas`'s own `onPaint` specifically rather than inferring
from the whole QML-load window.

**Net result:** first Qt frame improved from ~5.42s to ~4.47s kernel-clock
on eMMC (~0.95s, ~18%), overwhelmingly attributable to the systemd change,
not the QML-side work. Odometer on the fresh eMMC install reads the image's
default (`2347.0`/`0.0`) — expected, `emmc-install.sh` writes the whole
`.wic` including `/data`'s partition, and this bench board has no real
drive history to preserve.

### Round-2 serial verification: real power-on to first Qt frame, no proxy

The kernel-clock numbers above are real and internally consistent, but
every eMMC test in "round 2" was done over SSH/journalctl after the board
was already up — none of it re-establishes a genuine power-on `t=0` the way
the original "Boot-time measurement: first Qt frame" section's serial
captures did. Closed that gap with one more `measure-boot.sh` run, eMMC,
drain-to-2s-silence done by hand first (the "stale UART backlog" gotcha
that section documents — confirmed 0 bytes drained, so `t=0` here is
trustworthy, not contaminated):

```
falcon payload load      0.308s
ATF start                1.079s   (+0.771s)
first Qt frame            5.238s   (+4.159s)
```

`kernel entry`/`framebuffer ready` read "not seen" as expected (`quiet`
cmdline, per "Boot-time optimization" above) — not a failure. Full log:
`boot-logs/boot-20260811T122048.log`.

**Power-on → first Qt frame: 5.238s**, directly comparable to the original
two hardware runs (6.664s, 7.298s) with the same methodology, same
instrument, same landmark. **1.4–2.1s faster**, real number, not estimated
from a kernel-clock delta plus an assumed bootloader offset.

## Boot-time optimization, round 3: a second stale-udevd holdover, ~1.1s more (2026-08-11)

Round 2 fixed `ultima-app.service` waiting on `basic.target`'s
`systemd-resolved`/D-Bus. That left `ultima-app.service` starting at
kernel_ts 2.913s (see "Final eMMC comparison" above). Going back to ask
"what's left to trim in the pre-app path" surfaced a second, bigger bug in
the same family.

### The finding

`ultima-data-mount.service` carries `After=systemd-udevd.service
Wants=systemd-udevd.service`, and `ultima-app.service` carries the same pair
(plus its real dependency on `ultima-data-mount.service`). Both looked
reasonable — the data-mount unit mounts a block device, udev feels like the
obvious prerequisite. **It isn't.** `ultima-data-mount.sh` derives `/data`'s
device from `findmnt -no SOURCE /` and does simple string substitution
(`mmcblk0p2` → `mmcblk0p3`) — a raw device node, deliberately never a udev
`by-uuid`/`by-label` symlink (see that script's own comment: SD and eMMC are
`dd`'d from the same image, so UUID/LABEL would collide the moment both are
visible at once). Checked the actual boot log for when that raw node
appears:

```
[    0.479902] beagleplay-ti kernel: mmcblk0: mmc0:0001 TB2916 14.6 GiB
[    0.481564] beagleplay-ti kernel:  mmcblk0: p1 p2 p3
```

The kernel's own MMC block-layer partition scan creates `/dev/mmcblk0p3` via
devtmpfs at **kernel_ts 0.48s** — driver-probe time, nothing to do with
udev — over a second before `systemd-udevd` even starts (`Starting
Rule-based Manager for Device Events and Files...` doesn't appear until
~2.23s in the same boot, and it doesn't finish until ~2.70s). `/data`'s
mountpoint directory is baked into the rootfs at build time too (`install -d
${D}/data` in `ultima-app.bb`), not created by `systemd-tmpfiles` at
runtime, so there's no other hidden prerequisite hiding behind the udevd
wait either. This is the exact same shape as the `dev-fb0.device` trap
`ultima-app.service`'s own comment already warns about (tidss being
kernel-built-in makes the framebuffer device exist via devtmpfs before
systemd even starts) — just not yet found for udevd specifically.

`ultima-app.service`'s own `After=systemd-udevd.service` had even less
justification: its own comment already flagged this as "very likely a
holdover from when tidss was still a module," CAN self-retries
(`CanBus::tryConnect()`), and it was never acted on.

### Fix and verification (hardware, not yet ported through a full Yocto rebuild)

Tested with the same fast-iteration technique used for the app binary, but
on the two unit files instead: remounted `/` rw over SSH, hand-edited both
`/usr/lib/systemd/system/{ultima-data-mount,ultima-app}.service` in place to
drop `systemd-udevd.service` from `After=`/`Wants=` (keeping everything
else, including `ultima-app.service`'s real `After=Wants=ultima-data-mount.service`),
`systemctl daemon-reload`, rebooted.

| | Round 2 (before) | Round 3 (after, hand-edit) |
|---|---|---|
| `ultima-data-mount.service` starts | kernel_ts 2.746s | **kernel_ts 1.860s** |
| `ultima-app.service` starts | kernel_ts 2.914s | **kernel_ts 2.250s** |
| first frame rendered | kernel_ts 4.470s | **kernel_ts 3.09–3.34s** (two reboots, run-to-run variance) |

Zero failed units, zero ordering cycles, both reboots. `local-fs.target`
itself is now reached (3.376s) *after* first frame — direct confirmation
the app was never waiting on anything else in that target, only on this
one stale `After=`.

**Odometer-persistence correctness re-checked** (this is the exact ordering
area that produced false-positive failures in round 2 — see "Odometer-
persistence correctness test" above): `systemctl stop ultima-app`, wrote a
distinctive test value (`totalOdo: 99999, tripOdo: 42`) to
`/data/odometer.json`, rebooted with the service left enabled. Value
survived intact, and the app's own journal confirmed it loaded (not
defaulted) and re-saved it: `OdoStore: saved totalOdo=99999.0
tripOdo=42.0`. Clean.

**Source ported** (`ultima-app.service`, `ultima-data-mount.service` in
`meta-ultima-beagleplay-src/recipes-ultima/`) with the reasoning above
recorded in each unit's own comment.

### `build.sh` doesn't pull the built image out — a stale-flash trap

Rebuilt from the ported source (`./build.sh`, all 8231 tasks succeeded, 2
benign "tainted from a forced run" warnings, both pre-existing) and flashed
to SD to verify. First attempt still showed the *old*
`After=systemd-udevd.service` on the board after flashing. Root cause:
**`build.sh` only runs bitbake inside the Docker volume — it does not copy
the resulting `.wic.xz` out to the host.** Its own last line says so
("Pull images out with the docker cp command..."), easy to miss since nothing
errors. `flash.sh` flashes whatever's already sitting in `deploy-falcon/` on
the host, so it silently re-flashed the stale round-2 image (confirmed by
file mtime: `deploy-falcon/tisdk-base-image-beagleplay-ti.rootfs.wic.xz` was
timestamped from the earlier eMMC-push build, hours before this rebuild
finished). Fixed by actually running the `docker cp`-equivalent command from
"Build environment" above before flashing. **Lesson: after `./build.sh`,
always re-pull the image before flashing — a successful build log proves
nothing about what's sitting in `deploy-falcon/`.**

### Verified from the real from-source build (not the hand-edit), SD

| | kernel_ts |
|---|---|
| `/data` mount starts → finishes | 2.042s → 2.336s |
| `ultima-app.service` starts | 2.415s |
| `app main()` entered | 2.857s |
| first frame rendered | **3.829s** |

Zero failed units, zero ordering cycles. (SD is slower than eMMC per the
established pattern elsewhere in this file — not directly comparable to the
round-2 eMMC numbers; the eMMC comparison is still pending, see below.)
Odometer-persistence re-verified a second time on this actual from-source
build (not just the earlier hand-edit): `systemctl stop ultima-app`, wrote
`totalOdo: 88888, tripOdo: 7`, rebooted with the service enabled — value
survived, `OdoStore: saved totalOdo=88888.0 tripOdo=7.0` in the journal.
Clean.

### Pushed to eMMC, final eMMC comparison

`emmc-push.sh` from the SD-booted board (first attempt correctly used the
freshly re-pulled `.wic.xz`, not the stale one from the earlier trap above).
Clean install, zero failed units, zero ordering cycles on the production
device:

| | Round 2 (eMMC) | Round 3 (eMMC, from-source) |
|---|---|---|
| `/data` mount starts → finishes | 2.746s → 2.852s | **1.891s → 2.178s** |
| `ultima-app.service` starts | 2.914s | **2.261s** |
| first frame rendered (kernel_ts) | 4.470s | **3.338s** |

### Round-3 serial verification: real power-on to first Qt frame

Same instrument, same methodology as every other number in this file:
drain-to-2s-silence confirmed (0 bytes) with the board powered off, then a
real power-on `t=0` via `measure-boot.sh`:

```
falcon payload load      0.308s
ATF start                1.078s   (+0.770s)
first Qt frame            4.536s   (+3.457s)
```

**Power-on → first Qt frame: 4.536s.** Down from the round-2-confirmed
**5.238s** (0.70s faster, matching the kernel-clock delta above almost
exactly), and **2.13–2.76s faster than the original 6.664s/7.298s
baseline** this whole investigation started from. Full log:
`boot-logs/boot-20260811T135201.log`.

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
is qmake/Qt5/QML (`ultima-app/`, shared and
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
- **No usable GPU driver here either, at the time** (same PowerVR AXE-1-16M
  situation as the now-removed Buildroot BeaglePlay port). qtbase's default
  `PACKAGECONFIG_GL` pulls in `eglfs kms gbm gles2` whenever `DISTRO_FEATURES`
  has `opengl` without `x11` (true for arago's distro conf) — overridden via
  `recipes-qt/qt5/qtbase_git.bbappend`, scoped `:beagleplay-ti` only (a
  global override was already proven to break unrelated recipes' QA checks
  earlier in the Falcon work above — same lesson, reapplied), to
  `PACKAGECONFIG:remove = "eglfs kms gbm gles2"` +
  `PACKAGECONFIG:append = "no-opengl linuxfb"`. **Verified**: `qtbase`
  builds clean with this, `qtbase-plugins` contains `libqlinuxfb.so`, no
  `eglfs`/eglfs-adjacent output anywhere in its work directory.
  **Superseded (2026-08-12): see "PowerVR GPU enablement" further down** —
  meta-ti's BSP turns out to already be fully wired for GPU on this exact
  machine via TI's own rogue DDK; this bbappend now keeps the GL path and
  adds `linuxfb` alongside it instead of removing it.
- `QT_QUICK_BACKEND=software` is a runtime env var (set in the systemd
  unit below), not a build-time option — no qtdeclarative PACKAGECONFIG
  change needed for it.

**ultima-app recipe** (`recipes-ultima/ultima-app/ultima-app.bb`): builds
the exact same source tree as the Buildroot RPi5/BeaglePlay builds
(`ultima-app`) rather than a duplicated copy —
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
module-load race with early CAN access).

**Superseded (2026-08-20):** the ODrive USB-CAN adapter was retired in favor
of a mikroBUS-mounted SPI CAN controller — see "CAN interface migrated from
USB adapter to mikroBUS SPI click" further down. `ultima-can.cfg` no longer
has `CONFIG_CAN_GS_USB` at all.

`CONFIG_DRM_TIDSS`/
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
and nothing in this image otherwise pulled it in. **Superseded (2026-08-20)
— see "CAN interface migrated from USB adapter to mikroBUS SPI click"
further down**; `br2-external/` (and the Buildroot RPi5 port it belonged to)
no longer exists in this repo either way, so that mirrored-from path is
stale regardless of the CAN hardware change.

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

`dropbear` is in this image by default. **Stale as of 2026-08-19 — see
"WiFi AP + captive portal abandoned, reverted to client mode" below**: WiFi
went through several shapes since this paragraph was first written —
disabled, then a Skynet STA client, then briefly a standalone AP with a
captive portal, and as of 2026-08-19 it's back to a Skynet STA client (the
AP/captive-portal work was fully reverted, not just paused). SSH today
reaches the board either over wired Ethernet with a DHCP lease (`cpsw`,
plausible arago default, not confirmed) or over WiFi once
`/data/wifi-client.conf` has been provisioned with the Skynet credentials
(see that section) — there's no fixed `192.168.4.1` to fall back to anymore
now that AP mode is gone. `journalctl -u ultima-app` over the serial console
is still the fallback if neither network path is up.

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

**Follow-up (2026-08-19):** the "harmless cosmetic blip" above (dash briefly
shows the stale boot-default time, e.g. 6:48 PM, before self-correcting)
still visibly happened on hardware — decided it was worth masking on the
display side rather than continuing to accept it, since that doesn't touch
the boot-ordering tradeoff made above. Added `SystemClock::timeIsValid()`
(`systemclock.h`/`.cpp`). `clockText` in `main.qml` shows `--:--` until it
flips true, then shows the real time on the same per-second `Timer` tick
that was already re-reading `new Date()`. No change to unit ordering or
boot latency.

First implementation compared the wall clock against this binary's own
build time (`__DATE__`/`__TIME__`), on the theory that the stale
boot-default is always earlier than build time and real time never is.
**Deployed via the hot-deploy loop and immediately regressed to showing
`--:--` permanently** — root cause: the Docker build container's clock had
drifted ~5 hours ahead of real time (an OrbStack VM clock-drift case,
likely from the Mac sleeping/waking; container read `22:38:52 UTC` against
the board's actual, correct `17:38:33 UTC` at the same moment), so the
embedded "build time" was itself hours in the future relative to real time
— `timeIsValid()` couldn't flip true until the real clock caught up to that
bogus future timestamp. The deeper problem wasn't the drift itself but the
design: **no build machine's clock is trustworthy ground truth**, so
comparing against one was never going to be reliable, drift or not.

Fixed by reading `/dev/rtc0` directly instead — the same clock
`ultima-hwclock-load.service` itself trusts, so it can't disagree with the
thing that's supposed to correct the system clock in the first place.
`timeIsValid()` now opens `/dev/rtc0`, `RTC_RD_TIME`-reads it, and compares
against `time(nullptr)`; fails closed (reports invalid) if the device isn't
there yet or the read fails, which correctly covers the brief window right
after boot where the app is already running but `bq32k` hasn't registered
`rtc0` yet. Re-deployed and confirmed via SSH: fresh `ultima-app` restart,
system clock already RTC-correct, no `__DATE__`/`__TIME__` string left in
the binary (`strings` confirms). Lesson for next time a "is this value
sane" check is needed: prefer comparing against another value already
proven live on the *target* device over anything baked in from the build
side, even something as seemingly-safe as a compile timestamp.

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
C++, anything under `ultima-app`) leaves every
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
scp /tmp/hotdeploy/ultima-app root@beagleplay-ti.local:/tmp/ultima-app.new
ssh root@beagleplay-ti.local '
  mount -o remount,rw / &&
  mv /tmp/ultima-app.new /usr/bin/ultima-app &&
  mount -o remount,ro / &&
  systemctl restart ultima-app
'
```
Overwriting `/usr/bin/ultima-app` in place while it's running fails with
`ETXTBSY` (scp's sftp backend reports it as a generic "dest open" failure) —
upload to a temp path and `mv` over it instead; renames don't hit that,
since the running process keeps its own reference to the old inode. This
whole loop is minutes and needs zero physical access to the board, versus a
full image build + card swap + held-button power-on. Doesn't help for
kernel/DT/bootloader changes, only app source.

**Updated 2026-08-11 for read-only-rootfs** (see "Read-only rootfs" below,
which shipped after this loop was first written): the original version
staged the upload in `/root` and moved it into place directly, both of
which are on the now-`ro` root partition and fail outright. Stage in `/tmp`
(tmpfs) instead, and bracket the move with `remount,rw`/`remount,ro` — same
pattern as the live unit-file-edit trick in "Read-only rootfs" → "Two
regressions...". `remount,ro` failed once with `mount point is busy`
(some transient writer, not root-caused — `fuser -m /` wasn't useful, it
lists nearly every PID since almost everything maps something off root).
Don't fight it by hand: the binary swap had already landed by that point
(verify with `md5sum`), so a plain `reboot` over SSH resolves it safely —
systemd's own shutdown sequence remounts read-only as part of normal
shutdown, succeeding where a live remount attempt didn't. Confirmed
afterward: `findmnt` back to `ro`, binary md5 unchanged, service active.

**Update (2026-08-19):** re-ran this loop for the dash-clock `--:--` fix
(`systemclock.cpp`'s `timeIsValid()`) and hit `mount point is busy` on
*both* directions again — first `remount,rw` (worked on retry), then
`remount,ro` (didn't clear on retry this time). **This is the common case,
not the rare one this section originally implied** — two-for-two sessions
now. Also re-confirmed the `/tmp/ultima-app.new` disappearing-between-calls
gotcha above: a diagnostic `ls` between the `scp` and the `mv` was enough to
lose the staged file, so keep `scp` and the `mv` in the same immediate `ssh`
call with nothing in between, every time, not just when convenient.
`reboot`-as-fallback for the stuck `remount,ro` worked again, cleanly
(`findmnt` back to `ro`, `md5sum` unchanged, service `active`, clean
restart in the journal) — still the right move rather than fighting it by
hand, but budget for needing it on essentially every hot-deploy, not as an
edge case. Root cause of what's holding `/` open still not found.

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
persistence across reboots, WiFi (not wired up at all in this build at the
time — since enabled, see "WiFi AP + captive portal abandoned, reverted to
client mode"). Boot-time measurement with the full chain is now done — see
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

**Update 2026-08-10**: root itself is now read-only end to end — see
"Read-only rootfs" below — so this specific risk no longer applies to root.
The same caution still applies to `/data`, the one partition still mounted
read-write.

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

## Read-only rootfs (2026-08-10) — done, hardware-verified on SD and eMMC

Answers the question that started this: a gauge cluster gets its power cut
constantly, with no clean-shutdown warning, every time the car turns off.
Root is now mounted `ro` and rejects writes outright, so there's no dirty
ext4 state for a power cut to leave behind on the partition Falcon actually
boots from — "First hardware attempt: the crash-loop and its risk" above
(this rootfs has no read-only protection...") and the PARTUUID/superblock
war stories under "Always shut down cleanly" below were both about exactly
this failure class on the *old* read-write root. `/data` (p3, see "Data
persistence" — odometer state) is still a real read-write ext4 partition
and still needs a clean shutdown to avoid corruption; it just isn't root,
so a torn write there can't take the whole board down with it, and
`OdoStore::save()`'s atomic-write-plus-fsync (see `odostore.cpp`) covers
its own worst case directly.

**What's in place**, layered on top of the `/data`-partition groundwork
(`wic/ultima-beagleplay.wks.in`, journald/coredump on tmpfs, `resize_rootfs`
masked — all from the same effort, done first and separately):

- `IMAGE_FEATURES += "read-only-rootfs"` in `tisdk-base-image.bbappend`.
  oe-core's own `rootfs-postcommands.bbclass` (`read_only_rootfs_hook`)
  handles most of the mechanics automatically once this is set — read the
  class before assuming any of this needs custom work:
  - appends `ro` to the kernel cmdline itself (`APPEND:append`);
  - rewrites `/etc/fstab`'s `/dev/root` line to `ro` (matched this image's
    fstab as shipped, no `.wks.in` change needed);
  - empties `/etc/machine-id` at build time so systemd's transient-ID
    support takes over — a fresh ID every boot, not persisted;
  - detects no baked-in `/etc/dropbear/dropbear_rsa_host_key` and points
    dropbear at `/var/lib/dropbear` instead, generated fresh every boot.
    Deliberately left transient rather than persisted onto `/data` — this
    project already tolerates dropbear host-key churn on every reflash
    (see `emmc-push.sh`'s `SSH_OPTS` comment), so a boot-to-boot version of
    the same churn isn't a new cost.
- `volatile-binds` (stock oe-core, not pulled in by any TI/arago layer by
  default) added to `IMAGE_INSTALL`, giving `/var/lib`, `/var/cache`,
  `/var/spool` and `/srv` a tmpfs overlay — the one thing the hook above
  assumes exists and this distro doesn't provide on its own. Without it,
  dropbear's redirected key location has nowhere writable to land.
- Checked directly against hardware rather than assumed clean: `/tmp` is
  already tmpfs via systemd's static `tmp.mount`, and
  `ultima-hwclock-load`'s `hwclock --hctosys` does not write
  `/etc/adjtime` on this build (confirmed absent after 5+ minutes of
  uptime) — neither needed any change.
- `docker-moby`/`containerd`/`netperf`/`lldpd`/`psplash` are still
  *installed* (only service-disabled, see `recipes-ultima/boot-trim`) —
  under `read-only-rootfs`, any postinst deferred to first boot fails
  `do_rootfs` outright rather than silently misbehaving at runtime. All
  five built clean on the first attempt; this was the actual discovery
  mechanism for that risk, not a review of each package by hand.

### Two regressions that only showed up on a real boot, not at build time

**1. Systemd dependency cycle from a missing `DefaultDependencies=no`.**
`ultima-data-mount.service` (mounts `/data` from whichever disk root
itself booted from — see "Data persistence") declares
`Before=local-fs.target`. Every `.service` also gets an implicit
`After=sysinit.target` by default — and `local-fs.target` itself precedes
`sysinit.target` in the normal boot chain, so that's a genuine ordering
cycle. systemd breaks cycles by deleting an arbitrary job rather than
refusing to boot, and on hardware it dropped
`systemd-tmpfiles-setup.service`'s start job instead of this unit's. That
meant `/var/volatile/tmp` and `/var/volatile/log` (created by a
`tmpfiles.d` rule this service was supposed to run before) never existed,
which took down `dbus-broker`'s `PrivateTmp=` (`EROFS` trying to
bind-mount a directory that isn't there), cascading into `avahi-daemon`,
`systemd-logind` and `systemd-resolved` all failing. Confirmed by
`journalctl -b | grep -i "ordering cycle"` — this unit named in every
cycle chain systemd printed. `volatile-binds`' own service template sets
`DefaultDependencies=no` for exactly this reason; the fix here was
noticing that and copying it, having first (wrongly) judged it
unnecessary boilerplate.

Fast way to validate an ordering fix like this without burning a full
build+flash cycle: the board is already up, root is `rw` until you say
otherwise —
```
mount -o remount,rw / && <edit the unit under /usr/lib/systemd/system/> \
  && mount -o remount,ro / && reboot
```
Confirms in one reboot; a clean rebuilt-and-reflashed image is still the
final check, since a hand-edited unit doesn't prove the build pipeline
produces the same file.

**2. `emmc-push.sh`/`emmc-install.sh` staged the transferred image in
`/root`**, which is now part of the read-only root partition — running
`emmc-push.sh` against a read-only-rootfs SD boot failed immediately with
`Read-only file system` on the very first file copy. Moved staging to
`/tmp` (tmpfs): board has ~1.8GB free RAM against the ~105MB payload, and
nothing staged there needs to survive a reboot — `emmc-install.sh` runs
immediately after the copy and the board powers off right after it
succeeds.

### Verification checklist (what "working" looks like)

```
findmnt -no SOURCE,OPTIONS /        # mmcblk{0,1}p2, ro,relatime
touch /x                             # must fail: Read-only file system
journalctl -b | grep -i "ordering cycle"   # must be empty
systemctl --failed                   # must be empty
ls -d /var/volatile/tmp /var/volatile/log  # both must exist
findmnt /data                        # mmcblk{0,1}p3, rw,relatime
systemctl is-active dbus-broker avahi-daemon systemd-logind \
  systemd-resolved ultima-app ultima-data-mount   # all "active"
cat /data/odometer.json              # real content, not the hardcoded default
ls -la /var/lib/dropbear/            # key regenerated this boot
```
Ran clean on both SD (`mmcblk1p2`/`mmcblk1p3`) and eMMC
(`mmcblk0p2`/`mmcblk0p3`) from a real flash, not a live-patched boot.

### Not done, not currently planned

- `/data`'s ext4 UUID being identical across SD and eMMC (both flashed
  from the same build) is no longer a mount hazard — `ultima-data-mount`
  derives the device from the root filesystem instead of matching by UUID
  — but the UUIDs themselves are still equal. Harmless as things stand;
  would only matter if something else ever needed to tell the two `/data`
  partitions apart by filesystem identity.
- A deliberate power-yank stress test (repeatedly cutting power to the
  running board) would be the direct empirical close-out of the question
  that started this, but wasn't requested — the design argument above
  (root literally rejects writes) was judged sufficient without it.

## Always shut down cleanly — `sync; poweroff`

This section predates "Read-only rootfs" above and both incidents below hit
root while it was still read-write — root can no longer be dirtied by a
power cut at all. Still applies as written to `/data` and to anything
running from the SD card during flashing/install work, both of which stay
read-write.

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

- **The host key changes on every single boot, not just on reflash** —
  originally thought this was a reflash-only thing (`dropbearkey` generating
  a fresh one on first boot), but a hot-deploy session on 2026-08-19 hit
  `REMOTE HOST IDENTIFICATION HAS CHANGED` twice from two plain `reboot`s of
  the *same* card, with two different fingerprints. Means dropbear's host
  key file isn't persisted anywhere writable under the read-only rootfs (see
  "Read-only rootfs" below) — it's regenerated fresh into ephemeral storage
  every boot, not just at first-boot-after-flash. Combined with the SD and
  eMMC systems both calling themselves `beagleplay-ti`, pinning host keys is
  pointless here. The scripts already pass
  `-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null` for exactly
  this reason — prefer that over a one-off `ssh-keygen -R` before every
  connection, since "every boot" makes the manual version tedious fast.
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
the Mac does the staging and prints the exact board-side line. Wired
ethernet was the only network path when this was written; as of 2026-08-18
the board also runs its own WiFi AP (see "WiFi AP" below), which a Mac
could join instead if it's ever more convenient than ethernet.

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

## Boot splash investigation (2026-08-11): fbcon's built-in logo doesn't render

**Goal**: show something on screen before the gauge cluster starts, without
adding meaningful boot time. Two candidate mechanisms: the kernel's built-in
fbcon/`CONFIG_LOGO` boot logo (in theory free — piggybacks on driver init
that already happens), or a small userspace program that blits an image
straight to `/dev/fb0`, started unordered relative to `ultima-app.service`.

**fbcon logo: config is already fully enabled, but it doesn't produce
pixels.** Checked directly against the running kernel's `/proc/config.gz`,
not assumed:

```
CONFIG_FRAMEBUFFER_CONSOLE=y
CONFIG_FRAMEBUFFER_CONSOLE_DETECT_PRIMARY=y
# CONFIG_FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER is not set
CONFIG_LOGO=y
CONFIG_LOGO_LINUX_CLUT224=y
CONFIG_VT=y
CONFIG_FB=y
CONFIG_DRM_FBDEV_EMULATION=y
```

Deferred-takeover being off matters: it rules out the usual "logo doesn't
show until first console write" gotcha. `dmesg` confirms fbcon actually
binds to the real hardware framebuffer, not a placeholder:

```
[   0.517256] [drm] Initialized tidss 1.0.0 for 30200000.dss on minor 0
[   0.773285] Console: switching to colour frame buffer device 200x45
[   0.797765] tidss 30200000.dss: [drm] fb0: tidssdrmfb frame buffer device
```

Only one `fb0:` registration ever appears (no earlier `simple-framebuffer`
to rebind away from), so a detect-primary rebind-to-a-blank-device theory
doesn't hold up either.

**Empirical proof it's not rendering** (masked `ultima-app.service` first so
nothing else could touch the framebuffer, removing any race):

- `dd if=/dev/fb0 bs=6400 count=720` (full 1600×720×32bpp frame, stride 6400)
  right after a clean boot → **0 nonzero bytes out of 4,608,000**. Not "hard
  to see" — genuinely never drawn.
- `echo hello > /dev/tty1` (forces a real console write, the "first output"
  trigger) then re-dumped fb0 → **426 nonzero bytes**. That's consistent with
  just the text glyphs (a handful of characters × ~8×16px cells, mostly
  background), not a logo — a small CLUT224 image would be tens of thousands
  of bytes minimum. So the fbcon→fb0 pixel path genuinely works (proven with
  real text pixels), but the dedicated `fb_show_logo()` boot-logo blit
  specifically never fires on this kernel/DRM-driver combination. Not chased
  further into fbcon source — likely a known-ish incompatibility between the
  classic fbcon logo path and DRM's generic fbdev-emulation helpers on a
  modern (6.12) kernel, not something specific to this board's config.
- Also found for real along the way, independent of the logo question:
  `getty@tty1` was active and writing a login prompt to the same console —
  `agetty -o "-p -- \u" --noclear` — which would visually intrude on any
  splash regardless of the logo mechanism. Masking it
  (`systemctl mask getty@tty1`) is straightforward and doesn't affect serial
  or SSH access.

**Verdict**: the free/zero-cost option doesn't work as shipped. The
remaining path is a small userspace program that `mmap`s `/dev/fb0` and
blits a raw image directly, launched unordered relative to
`ultima-app.service` (proven writable, see above). Panel facts to design
around, all confirmed on hardware:

| Fact | Value |
|---|---|
| Resolution | 1600×720 |
| Bits per pixel | 32 (`rgba 8/16,8/8,8/0,0/0` — XRGB8888) |
| Stride | 6400 bytes/line |
| Online CPUs | 4 (fbcon tiles one logo copy per CPU — moot for a custom blitter, but size art near full panel width if this project ever revisits fbcon) |
| fbcon binds to real hw fb | kernel_ts 0.773s |
| First Qt frame (eMMC, round 3) | kernel_ts 3.338s |
| Visible-window budget | ~2.5s, before anything else lands on screen |

**Rootfs is read-only** (see CLAUDE.md's BeaglePlay rules, corrected this
same session) — a live-hand-edit test needs `mount -o remount,rw /` first;
it reverts to `ro` on the next reboot since fstab itself wasn't touched.
`getty@tty1` and `ultima-app.service` were both masked live during this
investigation and unmasked again afterward — board confirmed back to a
verified-good cluster on screen before moving on.

## PowerVR GPU enablement (2026-08-12) — spike, not yet hardware-verified

Motivated by a question about compositing an RCA-video camera feed into the
gauge cluster, which raised whether GPU rendering was worth revisiting.
Every prior note in this file that calls the GPU unusable predates this
investigation and was written without actually reading the kernel source or
the Yocto build volume — this section corrects that.

**The premise "no usable GPU driver here" no longer holds, and inspecting
the volume shows the TI BSP is already fully wired for it on this exact
machine:**

- `meta-ti-bsp/conf/machine/beagleplay-ti.conf` requires `mesa-pvr.inc`,
  which sets `mesa-pvr` as the provider for `virtual/egl`,
  `virtual/libgles2`, `virtual/libgbm`, `virtual/mesa`.
- `PREFERRED_PROVIDER_virtual/gpudriver` already resolves to
  `ti-img-rogue-driver`, version-matched against `ti-img-rogue-umlibs` and
  `mesa-pvr` by `ti-bsp.inc` (`25.2.6850647` / `25.2.6850647` / `24.0.1`) —
  no hand-matching of repos required, meta-ti already did it.
- arago's `DISTRO_FEATURES` already has `opengl wayland vulkan`, no `x11`.
  meta-qt5's *default* `PACKAGECONFIG_GL` for a machine like that is already
  `kms gbm gles2 eglfs` — the only thing turning it off was this project's
  own `qtbase_git.bbappend`, confirmed by the built `qtbase/config.summary`
  showing `EGL .... no` / `OpenGL ES 2.0 .... no` before this change.
- The GPU device-tree node (`gpu@fd00000`, `compatible = "ti,am62-gpu",
  "img,img-axe"`) has no `status` property (= enabled by default) and the
  BeaglePlay `.dts` never touches it — confirmed in the *built* `.dtb`, not
  just source, via `dtc` inside the build container.

**Mainline `drm/imagination` was considered and rejected for this pass.**
`CONFIG_DRM_POWERVR=m` would enable the in-tree open driver — present in
this 6.12.57 tree, and its `of_device_id` match table
(`drivers/gpu/drm/imagination/pvr_drv.c`) contains exactly `"img,img-axe"`,
which the DT node's compatible list matches. But Qt 5.15's scenegraph needs
**GLES2**, and Mesa's upstream PowerVR support is a *Vulkan* driver only —
GLES arrives via Zink layered on top, which needs Mesa ~26.1 against this
build's stock 24.0.7 (confirmed via websearch, not in this volume). That's
a two-major backport that would also collide with `mesa-pvr`'s
`PREFERRED_PROVIDER` claims, versus the TI stack being one
`PACKAGECONFIG` line away. **`# CONFIG_DRM_POWERVR is not set` stays** in
`ultima-display.cfg` for this reason — see that file's own comment, and the
correction in "Boot-time optimization" above (the earlier claim that this
line was needed to fix `tidss` was wrong; it was never load-bearing for
that fix, and is kept now for a different, real reason: the in-tree driver
and TI's out-of-tree `pvrsrvkm.ko` would contend for the same GPU node).

**Changes made (not yet built or flashed):**

- `qtbase_git.bbappend`: dropped the `PACKAGECONFIG:remove`/`no-opengl`,
  keep `linuxfb` appended alongside the now-default GL set rather than
  replacing it — a same-image fallback if the GPU path regresses boot time
  or proves unstable (env-var-only revert, no rebuild — but revert is
  *both* `QT_QPA_PLATFORM=linuxfb` and `QT_QUICK_BACKEND=software`, not
  platform alone, since qtbase is now built `-opengl es2`).
- `tisdk-base-image.bbappend`: explicit `IMAGE_INSTALL` for
  `ti-img-rogue-driver ti-img-rogue-umlibs` (the RRECOMMENDS chain through
  `mesa-megadriver` should pull these in on its own, but explicit beats
  relying on a soft dependency) plus `kmscube mesa-demos` for smoke-testing
  the GPU independent of Qt — both come back out once the spike is
  confirmed working.
- `ultima-app.service`: `QT_QPA_PLATFORM=eglfs` +
  `QT_QPA_EGLFS_INTEGRATION=eglfs_kms`, `QT_QUICK_BACKEND=software` removed.
- `pvrsrvkm.ko` is **out-of-tree**, unlike built-in `tidss`, so it needs an
  explicit `/etc/modules-load.d/pvrsrvkm.conf` (installed by
  `ultima-app.bb`, mirrors the removed `tidss.conf` workaround) *and*
  `After=systemd-modules-load.service` on `ultima-app.service` — the `.conf`
  file alone doesn't order anything, since `systemd-modules-load.service`
  is itself `DefaultDependencies=no` and orders only against
  `sysinit.target`, which this unit (also `DefaultDependencies=no`,
  `WantedBy=local-fs.target` since round 3 above) has no relationship to.
  If that's still not early enough on real hardware, the fallback is
  restoring `After=/Wants=systemd-udevd.service` — the ~1.1s this project
  already paid once and removed in round 3.

**Pre-flight checked, not yet build-verified:** `mesa-pvr_24.0.1.bb` points
at a personal fork (`gitlab.freedesktop.org/StaticRocket/mesa.git`, branch
`powervr/24.0.1`); confirmed the pinned SRCREV
(`68af6a102c2298569e77d1aa8bccc1ff61438b3e`) is still fetchable via
`git fetch --depth=1` before starting anything else. Neither that fork nor
the `git.ti.com` rogue repos had ever been fetched into this volume before
this change — expect a multi-hour first build (arago also pulls in target
LLVM via `gallium-llvm` for `mesa-pvr` specifically), not the incremental
`build.sh` this project is used to.

**Open question, only settleable on hardware:** both `libgles2-mesa` (from
`mesa-pvr`) and `libgles2-rogue` (from the umlibs blob) package a
`libGLESv2.so.*`, and the umlibs recipe deliberately declines to
RPROVIDE/RCONFLICT the generic name so both can coexist in one rootfs.
Arago's own graphics packagegroup never installs a `*-rogue` package,
suggesting mesa-pvr's copy (linked against the blob behind the gallium
`pvr` driver, which installs as `tidss_dri.so`) is the one meant to win —
but neither the blob nor the mesa fork was ever fetched into this volume,
so this couldn't be confirmed statically. Check `ldd /usr/bin/ultima-app`
and the `eglinfo` renderer string once built.

**Verification plan, in this order (prove the GPU independent of Qt before
blaming the app — same lesson the tidss/fb0 crash-loop already taught
this project once):** build and check `qtbase/config.summary` → flash/boot
and check `lsmod`/`dmesg` for the DDK's `RGX Device registered BVNC` line →
`kmscube` + `eglinfo` renderer string → only then `ultima-app` →
`measure-boot.sh` against the round-3 baseline above (**3.338s to first Qt
frame, eMMC** — recorded for comparison, not gating this pass) → sanity
check the tach/boost `Canvas` elements specifically, since they were tuned
against the software backend (see "Candidate 2" above) and macOS dev builds
(GPU Qt6) have already proven a poor predictor of this target's behavior.

### Hardware verification (2026-08-12) — done, working end to end on SD

Built, flashed to SD (not eMMC — deliberately, so a bad spike doesn't touch
the production boot), and verified in the exact order the plan specified:
prove the GPU independent of Qt before blaming the app.

- `qtbase/config.summary`: `EGL`, `OpenGL ES 2.0`–`3.2`, `Vulkan`, `EGLFS`,
  `EGLFS GBM`, and `LinuxFB` (the fallback) all `yes` — previously all `no`.
- `do_rootfs` succeeded outright — the `read-only-rootfs` postinst-failure
  mode flagged as the likely first failure never happened.
- On the board: `pvrsrvkm` loaded, `dmesg` shows the exact DDK success line
  predicted from research — `Read BVNC 33.15.11.3 from HW device registers` /
  `RGX Device registered with BVNC 33.15.11.3` / `Initialized pvr
  25.2.6850647 for fd00000.gpu` / firmware `rgx.fw.33.15.11.3` loaded.
- `ultima-app.service`: **`NRestarts=0`** — the module-load-ordering fix
  (`After=systemd-modules-load.service`) worked on the first real boot, no
  crash-loop.
- `kmscube` + `eglinfo`, run independently with `ultima-app` stopped:
  `renderer: "PowerVR A-Series AXE-1-16M"`, `vendor: "Imagination
  Technologies"`, OpenGL ES 3.1 — genuine hardware acceleration confirmed,
  not a software rasterizer.
- **Settles the plan's one open question**: `/proc/<pid>/maps` for the
  running `ultima-app` shows it links mesa's public `libGLESv2.so.2.0.0` /
  `libEGL.so.1.0.0`, loads the gallium driver as `tidss_dri.so`, which in
  turn pulls in the vendor implementation as suffixed
  `libGLESv2_PVR_MESA.so.25.2.6850647` / `libsrv_um.so` / `libglslcompiler.so`
  / `libusc.so`. No naming collision — the two `libGLESv2` builds never
  contend, mesa-pvr's is what the app links, the blob sits behind it.
- Visual QA on the physical dash: correct, no artifacts, tach/boost `Canvas`
  elements fine.

**Boot-time cost, real serial capture** (`measure-boot.sh`, landmark's own
embedded kernel timestamp used rather than the capture's elapsed-since-arm
column — the capture's `t=0` was contaminated by a stale UART backlog byte
left over from the SSH session above, the same class of gotcha "Serial
automation lessons" already documents): **kernel_ts 7.025s to first Qt
frame**, vs. round-3's **3.338s** baseline — roughly **+3.7s**. Not a clean
comparison: this run is SD, round-3's baseline is eMMC, and rootfs reads
happen inside the measured kernel_ts window. A same-medium (eMMC) capture
would be needed to isolate the true GPU-only cost. One data point that
narrows it: the app's own self-timed `QGuiApplication` creation (EGL/platform-
plugin init) took +1.90s on the cold post-flash boot vs. +0.42s on a warm
`systemctl restart` — most of that delta is one-time GPU firmware/shader-
compiler cache warm-up, not storage.

### eMMC promotion (2026-08-12) — done, clean same-medium boot-time number

`emmc-push.sh` run end to end for the first time ever (NOTES.md's own
provenance notes had this and `emmc-install.sh` as codified-but-never-run as
of the original eMMC boot work) — pushed the same GPU-enabled `.wic.xz` this
section already verified on SD, plus the existing eMMC-variant SPL (no
U-Boot/falcon changes in this GPU work, so it didn't need rebuilding).
Completed and self-verified cleanly (md5 checks, superblock-vs-partition
size, signature distinctness, all as `emmc-install.sh` already does) on the
first real run. Board confirmed booting from `/dev/mmcblk0p2`, PARTUUID
`076c4a2a-02` (the known eMMC signature) with USR not held. `ultima-app`:
`NRestarts=0`, GPU registered clean, same as the SD verification above.

**Real, uncounfounded boot-time comparison**, same serial-capture method as
before (kernel_ts embedded in the "first frame rendered" log line, not the
capture's elapsed-since-arm column — same stale-UART-backlog artifact at
`t=0`, same fix): **kernel_ts 5.596s to first Qt frame, on eMMC**, vs.
round-3's **3.338s** baseline (also eMMC, same landmark) — a real **+2.26s**
GPU-path cost, this time isolated from storage medium. This also confirms the
SD run's caveat was real: SD hit 6.999s for the same landmark, so ~1.4s of
that number was genuinely storage speed, not GPU — the GPU-specific cost is
the ~2.26s figure, not the ~3.7s one.

**Not yet done:** `kmscube`/`mesa-demos` are still in the image
(`tisdk-base-image.bbappend`) and should come out before this ships, per the
plan. No decision yet on whether +2.26s to first frame is acceptable for a
car dash.

### GPU boot-time regression investigation (2026-08-12) — root-caused, both candidate fixes disproven

Investigated where the +2.26s GPU-path cost (above) actually goes, since
"acceptable for a car dash" needs a real answer, not a guess. All of this was
live SSH testing against the eMMC-booted board — no rebuild needed for any of
it, systemd drop-in overrides and env vars only.

**Two hypotheses**, motivated by this section's own earlier number (+1.90s
cold post-flash vs. +0.42s warm `systemctl restart` on `QGuiApplication`
creation): **H1**, one-time GPU kernel-module/firmware/HW init that recurs on
every real reboot regardless of anything userspace-controllable; or **H2**, a
userspace shader/DDK cache that could be persisted somewhere durable (this
board's rootfs is read-only — only `/data` survives a power cycle).

**Systemd ordering is not the bottleneck.** `journalctl -o short-monotonic`
on a real boot: `pvrsrvkm` module inserted at kernel_ts 2.098s,
`ultima-app.service` starts firing 130ms later at 2.228s. The
`After=systemd-modules-load.service` fix from the original GPU-enablement
work above is working exactly as intended and costs nothing extra.

**The regression is entirely inside the app process**, isolated by diffing
this session's self-timed `journalctl -u ultima-app` startup log against the
saved pre-GPU baseline (`boot-logs/journalctl-ultima-app-20260811T095X.txt`):

| Phase | linuxfb (old) | eglfs_kms (GPU) | Delta |
|---|---|---|---|
| `QGuiApplication created` | 0.16s | 1.12s | +0.96s |
| `QML loaded` | 0.72s | 1.85s | +1.13s |
| main() → first frame | 0.94s | 3.05s | +2.11s |

That +2.11s accounts for essentially the whole +2.26s kernel_ts regression.

**H2 (shader-cache fix) tested and disproven.** Pointed
`MESA_SHADER_CACHE_DIR` (and, as a free follow-up, the legacy
`MESA_GLSL_CACHE_DIR` alongside it) at `/data/mesa_shader_cache` via a
systemd drop-in (`mount -o remount,rw /`, then
`/etc/systemd/system/ultima-app.service.d/override.conf`). Confirmed the env
var actually reached the process (`/proc/<pid>/environ`), then confirmed the
cache directory stayed empty after multiple app restarts — this driver
stack's proprietary GLSL compiler (`libglslcompiler.so`/`libusc.so`, from
`ti-img-rogue-umlibs`) doesn't honor Mesa's generic disk-cache mechanism. No
`powervr.ini`/apphint config file exists anywhere on the rootfs either
(searched `/`), so there's no discoverable vendor-side caching knob either.
Also no `ShaderEffect` anywhere in the QML (checked all of
`ultima-app/qml/*.qml`) — only `Canvas` items, which raster on the CPU and
upload as a plain texture, so the `QML loaded` delta isn't custom
per-screen shader compilation; it's more likely Qt Quick's own small,
fixed set of built-in scenegraph material/glyph shaders, compiled once
against a slow embedded GLSL compiler.

**A second candidate looked like a dramatic fix and wasn't.** Pinning
`QT_QPA_EGLFS_KMS_CONFIG` to a static connector/mode (skipping `eglfs_kms`'s
DRM auto-probe) — connector `HDMI-A-1` @ `1600x720`, read from
`/sys/class/drm/card0-HDMI-A-1/`. On a warm `systemctl restart`,
`QGuiApplication created` dropped from +1.12s to +0.42s and `QML loaded`
from +1.85s to +0.58s — a huge apparent win. **A real reboot with the same
config still active showed zero improvement**: kernel_ts 5.511s to first
frame, `QGuiApplication created` back to +1.13s, `QML loaded` back to
+1.85s. The fast numbers were entirely a warm-restart artifact (DRM mode
already locked, GPU already clocked up from the prior process) — the exact
same trap as the original cold/warm confound this investigation set out to
explain. Confirms **H1**: this cost is tied to real hardware state (HDMI
link retrain, GPU power/clock domain bring-up, firmware boot handshake) that
resets on every genuine power cycle, not anything reachable from userspace
config.

**Conclusion: the +2.26s is a per-boot cost, not a per-image one, and it's
very likely intrinsic** to enabling GPU-accelerated rendering on this
hardware/driver combo — display link retrain plus GPU firmware/power-domain
cold-start, neither exposed as a tunable by TI's closed stack. Both cheap,
zero-rebuild candidate fixes are ruled out with hardware evidence, not
guesswork.

**Not yet tried, would need a real `ultima-app` rebuild to test (uncertain
payoff, see the no-`ShaderEffect` finding above):** deferring
`DiagnosticScreen`/`SetTimeScreen` instantiation — currently eager children
of `main.qml`, not behind a `Loader { active: false }` — to cut QML
parse/binding-evaluation cost specifically. That's a different cost than the
GLSL-compile cost this investigation isolated, and likely a smaller win
since `main.qml` itself already exercises the same `Canvas`/`Image`/`Text`/
custom-`FontLoader` built-in shader set the diagnostics screen would need.

**Loose end, not chased:** the first reboot attempted for this investigation
hit an unrelated anomaly — `xhci-hcd xhci-hcd.3.auto: can't setup: -110`
(USB controller probe timeout) stalled the boot for ~14s, landing first Qt
frame at kernel_ts 19.8s instead of ~5.5s. Didn't recur on any of the three
other reboots this session. Worth knowing about so a future one-off slow
boot doesn't get mistaken for a real regression — if it recurs, it's a USB
enumeration issue unrelated to anything in this section.

Live test artifacts (all removed from the board, rootfs restored to `ro`):
`/etc/systemd/system/ultima-app.service.d/override.conf`,
`/etc/xdg/eglfs_kms.json`, `/data/mesa_shader_cache/`. Both dead-end fixes
tried in this section left the repo untouched — but a third mechanism,
found afterward, did stick. See below.

### Partial fix applied: Qt's own shader/QML disk cache (2026-08-12)

The two dead-end fixes above were both about the *driver's* shader cache
(Mesa's, and — implicitly — anything the vendor blob might have had). Qt
itself has had a separate, unrelated disk cache since 5.9:
`QOpenGLProgramBinaryCache`, built on the standard GLES3
`glProgramBinary`/`glGetProgramBinary` API, nothing to do with Mesa. Qt
Quick's scenegraph was upgraded to route its built-in materials (exactly
what's driving the `QML loaded` delta this section isolated) through the
cacheable path. It only activates when `QStandardPaths::CacheLocation` is
writable — which resolves via `XDG_CACHE_HOME`, falling back to
`$HOME/.cache`. `ultima-app.service` set neither, so on this read-only
rootfs the cache was almost certainly silently resolving to a location it
couldn't write and never activating, on every boot since GPU enablement.

Live-tested first (`XDG_CACHE_HOME=/data/qt-shader-cache` via a systemd
drop-in, same zero-rebuild method as above): confirmed real writes this
time — both a `qtshadercache-arm64-.../` shader binary cache and, as an
unplanned bonus, Qt's separate QML bytecode cache
(`ultima-app/qmlcache/*.qmlc`). Verified across two independent real
reboots before trusting it, same discipline as the two disproven fixes:
`QML loaded` dropped from 1.85s to 1.65s and 1.66s. Real, but partial — my
read is that not every built-in Qt Quick scenegraph material routes through
the cacheable shader path in this Qt version (the custom-TTF glyph/text
rendering is the likely holdout), plus there's probably real per-boot
image-decode/texture-upload cost neither cache touches.

**Applied to the repo and hardware-verified on the actual packaged image**
(not just the live SSH test): `ultima-app.service` now has
`ExecStartPre=/bin/mkdir -p /data/qt-shader-cache` and
`Environment=XDG_CACHE_HOME=/data/qt-shader-cache`. Rebuilt
(`build.sh` — only 31 of 8957 tasks needed to rerun, confirming nothing
upstream of `ultima-app` was touched), pushed to eMMC via `emmc-push.sh`
(first re-run of that script since the original GPU-enablement work),
verified booting clean from `/dev/mmcblk0p2` with `NRestarts=0`. A follow-up
real reboot confirmed the fix holds on the properly-packaged image: `QML
loaded` at 1.71s, in the same 1.65–1.71s band as the two live-test reboots,
consistently below the 1.85s baseline. (That reboot's *total* main()→first-
frame number, 3.13s, looked slightly worse than the pre-fix 3.05s baseline
— traced to an unusually long final paint/swap gap, 0.204s vs. the normal
~0.09s, unrelated to this fix and not seen on the other two reboots —
ordinary boot-to-boot jitter, not a regression.)

**Net result: ~0.15–0.2s recovered from the ~2.26s GPU-enablement
regression.** Modest, but real, reproducible across three separate reboots,
zero downside, and now permanently in the image rather than a live-only
test.

## Boot splash implemented and hardware-verified (2026-08-12)

Follow-up to "Boot splash investigation" above, which ruled out the kernel's
fbcon logo (`fb_show_logo()` proven dead on this DRM-fbdev-emulation stack)
and concluded the remaining path was a small userspace program writing
directly to `/dev/fb0`. That conclusion predates GPU/`eglfs_kms` enablement,
which changed who owns the display — this pass designs around that and
verifies the result on real hardware, twice.

**Design: plain `/dev/fb0` writer, not libdrm/psplash-drm/Plymouth.**
Researched prior art first (STM32MP community threads use a `psplash-drm`
fork; upstream `psplash` has an RFC DRM backend; Plymouth is DRM-native but
heavier, with documented DRM-master-handoff bugs against vendor GPU blobs
elsewhere — relevant here since TI's `pvrsrvkm` is exactly that kind of
blob). Went with neither: a ~90-line C program
(`recipes-ultima/ultima-splash/files/ultima-splash.c`) that `mmap`s
`/dev/fb0` directly and blits a procedural test pattern (a ring — no real
logo art yet, that's a follow-up), using the exact pixel path the
2026-08-11 investigation already proved works (the `echo hello > /dev/tty1`
test). It never opens `/dev/dri/card0` and never touches DRM master at
all — which turned out to matter more than expected, see "Why the handoff
was clean" below.

**`getty@tty1.service` permanently masked.** The 08-11 investigation found
it writing a login prompt over the framebuffer and only masked it
live/temporarily. That's now permanent via `ultima_mask_getty_tty1()` in
`tisdk-base-image.bbappend`, same `ln -sf /dev/null` pattern already used
for `systemd-timesyncd`/`resize_rootfs`. Doesn't touch `serial-getty@` (a
separate template unit) — serial console access is untouched.

**New unit, ordered as early as this project's units get:**
`ultima-splash.service` — `Type=oneshot`, `DefaultDependencies=no`,
`WantedBy=sysinit.target`, `Before=ultima-app.service` (ordering only, not
a real dependency — both units reach systemd via their own `WantedBy=`).
No real prerequisites: it doesn't need `/data`, CAN, or anything
`ultima-app.service` waits on, so unlike that unit it isn't ordered against
`ultima-data-mount.service`. Being a `oneshot` matters here — it draws once
and exits; the image stays on screen because the display hardware holds
whatever was last committed, not because a process is holding it there. No
DRM master to hand off, no long-running process to synchronize a stop
against.

**Hardware-verified, two real power cycles (both USR-held SD boots, `build.sh` →
`flash.sh`, both `systemctl --failed` empty and `NRestarts=0` on `ultima-app`):**

Boot 1 (initial flash-boot): `ultima-splash` ran kernel_ts 2.024s–2.154s.
Notably *not* right after `tidss` binds (0.787s) — confirms this project's
own recurring finding elsewhere in this file that early boot is gated by
systemd's own startup overhead (parsing units, mounting basics), not by
which target a zero-dependency unit is `WantedBy=`. ~2.0s is apparently
about as early as *anything* can run here, splash included.

Boot 2 (supervised, with a live-only `QT_LOGGING_RULES=qt.qpa.*=true`
systemd drop-in on `ultima-app.service` for detailed KMS logging — removed
afterward, never in the shipped image):

| Event | kernel_ts |
|---|---|
| `tidss` fb0 ready | 0.787s (from boot 1's dmesg, unchanged) |
| `ultima-splash` starts / finishes | 2.130s / 2.226s |
| `ultima-app` `main()` entered | 3.145s |
| `qt.qpa.eglfs.kms`: "Atomic reported as supported" / "Atomic disabled" | 4.839s / 4.845s |
| `QGuiApplication created` | 5.366s (gated on GPU firmware load finishing ~4.2s — RGX firmware/shader binary load, unrelated to display) |
| **`qt.qpa.eglfs.kms`: "Setting mode for screen HDMI1"** | **7.252s** |
| `QML loaded` | 7.284s |
| first frame rendered / swapped | 7.446s / 7.464s |

**Why the handoff was clean (watched live, USR held): "saw the blue circle
and then straight into the gauge cluster" — no reported flicker or gap.**
Two things line up to explain it, neither of which was the original plan:

1. `ultima-splash` never opens `/dev/dri/card0` — it only writes through the
   DRM fbdev-emulation compat node. So it never contends for DRM master.
   The kernel-fbdev-emulation → `eglfs_kms` handoff this investigation was
   worried about *already happens on every boot regardless of the splash* —
   the splash only changes what pixels are sitting in the buffer when that
   handoff occurs, it doesn't add a new handoff.
2. This driver stack uses **legacy** KMS, not atomic (`eglfs_kms` probed
   atomic support and explicitly disabled it, kernel_ts 4.845s) — legacy
   `drmModeSetCrtc` does force a real modeset even when reusing the same
   mode, so a blank frame is expected. But Qt doesn't call it until
   **7.252s**, ~30ms before `QML loaded` and ~200ms before first frame —
   i.e. `eglfs_kms` doesn't set the mode early in its own startup, it waits
   until it's essentially ready to render. The ring is on screen
   continuously from 2.13s to 7.25s (~5.1s), and whatever single blank
   frame the modeset causes lands inside a ~200ms window immediately
   followed by real content, not as its own visible event.

**Net effect on the black-screen budget this was meant to fill:** was ~7.4s
of black-to-nothing on SD (this session's storage medium); now ~2.1s black
+ ~5.1s ring + a clean cut to the gauge cluster. Not a boot-time win (never
the goal here) — replaces dead black time with something intentional.

**Files:** `recipes-ultima/ultima-splash/{ultima-splash.bb, files/ultima-splash.c,
files/ultima-splash.service}`, `tisdk-base-image.bbappend` (added to
`IMAGE_INSTALL`, `ultima_mask_getty_tty1` added to
`ROOTFS_POSTPROCESS_COMMAND`).

**Not done:** real logo art (currently a procedural ring, chosen to prove
the mechanism first — same "prove it works before polishing" order as the
GPU spike above); not yet pushed to eMMC (SD-only so far, deliberately,
per this project's own pattern of spiking on SD before touching the
production boot source); `kmscube`/`mesa-demos` still need removing from
the image per the still-open GPU-enablement note above, unrelated to this
change.

### Real artwork swapped in, and promoted to eMMC (2026-08-12)

Replaced the procedural ring with the actual `ULTIMA RS` artwork
(repo-root `splash screen.png`, 1600×720 — already an exact match for the
panel, no scaling needed).

**Ships as a headerless raw pixel blob, not the PNG.** Decoding PNG
(DEFLATE + chunk parsing) on-target would mean adding libpng/zlib to a
read-only rootfs this project has otherwise kept deliberately dependency-
light (no libdrm either, see above) for a single static, known-at-build-time
image. Converted once, host-side, with Python/Pillow:

```
python3 -c 'from PIL import Image; \
  open("splash.rgbx","wb").write(
    Image.open("splash screen.png").convert("RGB").tobytes("raw", "BGRX"))'
```

`"BGRX"` isn't arbitrary — it's the same XRGB8888 layout this file already
confirmed against the panel's `fb_var_screeninfo` (red offset 16, green 8,
blue 0): on this little-endian target, 4 bytes in memory order [B, G, R,
pad] *is* a native `0x00RRGGBB` read. So `ultima-splash.c` needs zero
pixel-format conversion at runtime, just a straight per-row `read()` into
the mmap'd framebuffer (still stride-aware via `finfo.line_length`, same as
before) — the only new logic is a byte-count sanity check (refuses to draw
if the blob size doesn't match `xres*yres*4` for whatever panel is actually
attached, rather than guessing how to scale/crop it, same "refuse to
guess" stance as the existing bpp check). Ships to
`/usr/share/ultima-splash/splash.rgbx` (4,608,000 bytes raw; ~0.7MB added
to the compressed image — the large flat black background compresses
well). The source PNG stays checked in at the repo root as the thing to
re-run that conversion against; nothing regenerates it automatically.

**Hardware-verified on SD first** (same USR-held boot procedure as above):
real artwork rendered correctly — right colors, right orientation, no
row-order or channel-swap bugs, confirmed by direct visual check on the
board, not just log timestamps this time. Handoff still clean at this
image size (4.6MB raw is a lot more data than the ring's few hundred
touched pixels, but still well inside the ~5s window between splash-draw
and Qt's mode-set — no new timing risk observed).

**Promoted to eMMC** via `emmc-push.sh` (SD-booted board pushes over SSH,
verifies `mmcdev=0` on the SPL before writing, refuses to run if already
booted from eMMC) — first time this specific image has gone to the
production boot source rather than staying on the SD spike card. Confirmed
board was on `/dev/mmcblk1p2` (SD) before pushing.  `kmscube`/`mesa-demos`
removal (open item above) still not done — this promotion didn't wait on
it, same as the GPU-enablement eMMC promotion earlier didn't either.

## WiFi AP (2026-08-18)

WL1807 briefly ran as a WPA2 STA joining a home network ("Skynet," see git
history) the same day this was written, then got reconfigured to a
standalone AP instead (2.4GHz — see "5GHz doesn't work on this hardware
yet" below for why the original 5GHz plan didn't survive hardware testing)
— both changes landed within hours of each other. Not a toggle between the
two: WL1807 is single-radio (confirmed —
dual-band means tunable to 2.4 or 5GHz, not two independent radios running
both at once; TI's own WL1837 E2E forum thread documents the throughput
hit from time-slicing one radio across channels in "multichannel" AP+STA
mode), so being a Skynet client and being this board's own AP were never
both available simultaneously. AP-only was the explicit choice over
building a mode-switch mechanism.

`hostapd` (meta-oe) replaced `wpa-supplicant` for this. Config is split in
two, deliberately — the SSID/passphrase should never sit in repo history
the way Skynet's did. First draft tried hostapd's config-file `include=`
directive to pull the secret half in from `/data` at parse time; **wrong**,
caught before it shipped by an advisor review, then confirmed by grepping
hostapd 2.10's actual built source in the volume — `config_file.c` has no
`include=` config directive at all, only C preprocessor `#include`s. hostapd
also refuses to start on any unknown config line, so that draft would have
failed closed, but for the wrong reason (a syntax error, not "no
credentials yet"). Real mechanism:

- `/etc/hostapd.conf` — baked into the image (see
  `tisdk-base-image.bbappend`'s `ultima_enable_wifi_ap`), everything
  *except* the network identity: `interface=wlan0`, `driver=nl80211`,
  `hw_mode=g` + `channel=6`, WPA2-PSK/CCMP. No SSID/passphrase, no
  `include=`. **Originally `hw_mode=a` + `channel=36`** (5GHz, UNII-1 —
  deliberately non-DFS, since a DFS channel needs a Channel Availability
  Check, up to 60s/10min, before the AP can even come up — bad fit for
  "get in the car, connect a phone") — downgraded to 2.4GHz after live
  hardware testing rejected it outright. See "5GHz doesn't work on this
  hardware yet" below.
- `/data/wifi-ap.conf` — **not in git, not in the image at all** — two
  lines, plain hostapd config syntax:
  ```
  ssid=<network name>
  wpa_passphrase=<8-63 char WPA2 passphrase>
  ```
  Still hostapd syntax rather than JSON even now that this is a runtime
  assembly step, not a native parse — it's a plain `cat`, not a real
  merge, so the secret file's syntax has to already match what the
  assembled file needs. A future JSON-based settings UI would need to
  emit these two lines in this syntax, not write raw JSON here.
- `ultima-hostapd-config.service` — a new oneshot unit, `After=`/`Requires=
  ultima-data-mount.service`, that runs
  `test -f /data/wifi-ap.conf && cat /etc/hostapd.conf /data/wifi-ap.conf
  > /run/hostapd-wlan0.conf` at boot. `hostapd.service`'s own
  `ExecStart=` is overridden via a `.service.d` drop-in (blank
  `ExecStart=` first to clear the upstream one, then point at
  `/run/hostapd-wlan0.conf`), and that same drop-in adds
  `After=`/`Requires=` on both `sys-subsystem-net-devices-wlan0.device`
  and `ultima-hostapd-config.service` — upstream `hostapd.service` only
  ships `After=network.target`, no awareness of wlan0 or of the assembled
  config path.

  /data is partition 3, the one persistent writable partition on this
  board (see `ultima-data-mount.service`) — `wifi-ap.conf` has to be
  created once per device, by hand, over SSH or serial:
  ```
  ssh root@<board-ip-or-192.168.4.1> 'cat > /data/wifi-ap.conf' <<'EOF'
  ssid=...
  wpa_passphrase=...
  EOF
  systemctl restart ultima-hostapd-config hostapd
  ```
  (No `/` remount needed — unlike a live edit anywhere else in this repo's
  read-only-rootfs notes, `/data` is writable by design.) A settings screen
  to write this from the touchscreen, the same way
  `CalibrationSettingsScreen.qml` writes `calibration.json`, is planned but
  not built yet. Until then, or on a freshly flashed board with no
  `/data/wifi-ap.conf` yet: **fails closed, not open** — the assembler's
  `test -f` guard exits non-zero and writes nothing to `/run` when the
  secrets file is missing, and because `hostapd.service` `Requires=` that
  unit, a failed/skipped assembly run means hostapd never starts at all,
  rather than starting broadcasting with no password.

`10-wlan-ap.network` (static `192.168.4.1/24` + systemd-networkd's own
`DHCPServer=yes`, no dnsmasq needed) is named to sort ahead of the base
tisdk image's own `/etc/systemd/network/30-wlan.network` (`Name=wlan*`,
`DHCP=yes`) — both land in the same directory (confirmed against the
actual built rootfs, not just the recipe's source path), so it's a plain
lexical-sort win, no `/etc` vs `/run` vs `/usr/lib` precedence question in
play. networkd applies only the first matching `.network` file per
interface, so this one wins for `wlan0` and the base image's DHCP-client
default never applies to it.

5GHz AP mode also needs `wireless-regdb-static` in the image (added to
`IMAGE_INSTALL` alongside `hostapd`) — without `/lib/firmware/regulatory.db`,
cfg80211 stays in the permissive "world" regulatory domain, which flags
5GHz no-initiating-radiation. The earlier Skynet **STA** config never
surfaced this gap because a station passively adopts the AP's country IE
via 802.11d instead of needing its own regulatory database — only *hosting*
a 5GHz AP needs it locally. No crda daemon needed on this kernel (6.12) —
cfg80211 pulls `regulatory.db` in directly via the firmware-request path.
The `-static` suffix is load-bearing, not decoration: `wireless-regdb`'s
recipe (oe-core, `recipes-kernel/wireless-regdb`) splits `PACKAGES =
"${PN}-static ${PN}"` and marks the two `RCONFLICTS` — only `-static`'s
`FILES` include `regulatory.db`/`.db.p7s`; the plain `wireless-regdb`
package instead ships the older `regulatory.bin` for a crda daemon that
isn't in this image. First attempt specified bare `wireless-regdb` — a
validly-named package, so `do_rootfs` raised no error at all, and only
re-inspecting the built rootfs (not the build succeeding) caught that
`regulatory.db` still wasn't there.

### 5GHz doesn't work on this hardware yet — missing wl18xx firmware files

Live-tested over SSH on real hardware (wired-Ethernet session at
`192.168.50.220`, deliberately not the `beagleplay-ti.local` mDNS name,
so stopping `wpa_supplicant@wlan0.service` to free `wlan0` for hostapd
couldn't cut the session out from under itself) before committing to a
full rebuild+reflash cycle: installed the built `hostapd` binary +
`/etc/hostapd.conf` + `regulatory.db` + the new systemd units onto the
board by hand (`mount -o remount,rw /`, copy, `mount -o remount,ro /`
immediately after — every shared library `hostapd` needs, `libnl-3`,
`libnl-genl-3`, `libssl`, `libcrypto`, was already present from other
packages, nothing else to bring over), then wrote the real
`ssid=Ultima RS` / `wpa_passphrase=linkedlist` to `/data/wifi-ap.conf`.

With `hw_mode=a`/`channel=36` as originally designed, hostapd's journal
showed:
```
wlan0: IEEE 802.11 Configured channel (36) or frequency (5180)
  (secondary_channel=0) not found from the channel list of the current
  mode (2) IEEE 802.11a
wlan0: IEEE 802.11 Hardware does not support configured channel
```
and the service cleanly deactivated (not a crash-loop — no instability,
no repeated restarts, just a refusal to start). Root cause visible in
`dmesg` from the very first boot, unrelated to anything in this change:
```
wl18xx_driver wl18xx.6.auto: Direct firmware load for
  ti-connectivity/wl1271-nvs.bin failed with error -2
wl18xx_driver wl18xx.6.auto: Direct firmware load for
  ti-connectivity/wl18xx-conf.bin failed with error -2
wlcore: ERROR could not get configuration binary
  ti-connectivity/wl18xx-conf.bin: -2
wlcore: WARNING falling back to default config
```
`wl18xx-firmware` (this layer's own recipe) only ever vendored the one
firmware blob the driver's base probe needs — not the NVS calibration file
(`wl1271-nvs.bin`) or the `wl18xx-conf.bin` runtime config, and the
driver's fallback default config apparently doesn't expose an 802.11a
(5GHz) channel list at all. **Not yet root-caused further than that** —
open questions for whoever picks this up: does adding those two firmware
files unlock 5GHz outright, or is NVS calibration data board/antenna-
specific (common for WiLink parts) such that it can't just be vendored
generically the way the single base firmware blob was? Real follow-up
item, not attempted here.

Switched `hostapd.conf` to `hw_mode=g`/`channel=6` (2.4GHz) instead and
re-ran the same live test: `hostapd.service` went `active` (not
failed/deactivated), `wlan0` came up at `192.168.4.1/24` exactly as
configured, and `dmesg` showed a clean `deauthenticating ... by local
choice (Reason: 3=DEAUTH_LEAVING)` from the old Skynet BSSID as the
interface transitioned from STA to AP. Also removed the original image's
`wpa_supplicant@wlan0.service` wants-symlink from the live board directly
— it was still `WantedBy=multi-user.target` from the prior STA build and
would have raced hostapd for `wlan0` on the next reboot otherwise.
Confirmed after cleanup: `wpa_supplicant@wlan0.service` inactive with no
wants-symlink, `hostapd.service` active, root back to `ro,relatime`.

**A phone associating was the one gap this whole write-up left open — now
closed.** The AP-side checks from this Mac were inconclusive (`airport -s`
and `system_profiler SPAirPortDataType` both came back empty, most likely
a Location Services/permission restriction on the deprecated `airport`
tool on modern macOS, not anything about the AP itself), but a real phone
joining "Ultima RS" with `linkedlist` confirmed the whole path end to end:
`/data/wifi-ap.conf` → `ultima-hostapd-config.service` assembly →
`hostapd` on `hw_mode=g`/`channel=6` → a client actually associating.
`wireless-regdb-static` stays in the image regardless of band — harmless
for 2.4GHz-only operation, already in place for whenever 5GHz gets
unblocked.

**Build-verified and hardware-verified, phone included — no open gaps.**
`./build.sh tisdk-base-image` ran clean four times over the course of
getting this right — the `include=`, missing-regdb,
wrong-regdb-package-name, and 5GHz-doesn't-work mistakes were each only
found by inspecting the actual built rootfs or testing on real hardware
(the first two also needed an advisor review to even go looking), never
by the build itself reporting an error, which is worth remembering next
time this file's "build succeeded" gets read as "the logic is right."
Only pre-existing sstate-taint warnings appeared across all four runs,
unrelated to this change. Final pass's rootfs was inspected directly in
`arago-tmp-default-glibc/work/beagleplay_ti-oe-linux/tisdk-base-image/`:
every file landed exactly as written, no `wpa_supplicant` leftovers,
`hostapd` binary present at `/usr/sbin/hostapd`, `regulatory.db` +
`.db.p7s` present under `/usr/lib/firmware/`, `hostapd.conf` shipping
`hw_mode=g`/`channel=6`. Also confirmed `30-wlan.network`'s real location
is `/etc/systemd/network/`, not `/usr/lib/systemd/network/` as the
recipe's source path alone would suggest, and confirmed directly against
hostapd 2.10's source that `config_file.c` has no `include=` token.

## WiFi AP + captive portal abandoned, reverted to client mode (2026-08-19)

Built on the AP work above: a captive-portal page that auto-pops when a
phone joins "Ultima RS," handing off to a phone-facing settings UI
(`ultima-app/src/portalserver.h/.cpp`, a hand-rolled HTTP server; `dnsmasq`
in DNS-only role wildcarding every hostname on wlan0 to 192.168.4.1). This
was **abandoned and reverted**, not just paused — the whole feature and
its reasoning are described here for the record, not because any of it
ships.

**What actually worked, hardware-verified:** the network/HTTP plumbing
itself was solid. dnsmasq correctly wildcarded arbitrary hostnames
(`connectivitycheck.gstatic.com`, `www.google.com`, ...) to 192.168.4.1; a
real DHCP lease was handed out to a connecting phone; `PortalServer`
correctly answered `GET /` and `GET /settings` with 200s bound only to
192.168.4.1 (confirmed unreachable from the wired debug network by
construction — the socket table showed no wired-interface listener at
all). A live `tcpdump` capture during a real phone connecting showed real
`GET / HTTP/1.1` requests landing on the server.

**What didn't hold up: the actual captive-portal auto-popup UX.** Whether
an OS shows its "Sign in to network" prompt automatically is real,
per-OS-version, best-effort behavior — flagged as the real risk before any
of this was built (see the AP section's own hardware-verification gap
above, and this section's own earlier design discussion), and it bore out:
real-device testing didn't produce a clearly reliable auto-pop, and
iterating on it would have meant a cycle of destructive reflashes (see
below) per adjustment to figure out exactly what each OS's probe expected.
Decided not worth the cost — a Bluetooth link is the new plan for
whatever phone-facing settings/telemetry surface this project wants
instead, sidestepping the entire OS-captive-portal-heuristic problem.

**Reverted:** `portalserver.h/.cpp` deleted; `ultima-app.pro`/`main.cpp`
back to no `QtNetwork` dependency; `dnsmasq` and the AP's
`hostapd`/`wireless-regdb-static` both dropped from `IMAGE_INSTALL`;
`10-wlan-ap.network`'s custom `[Network]`/`[DHCPServer]` config removed.
WiFi is back to `wpa_supplicant` STA-to-Skynet client mode — see
`tisdk-base-image.bbappend`'s current `ultima_enable_wifi()` comment for
the mechanism. Unlike the very first STA-to-Skynet attempt (which baked
`ssid`/`psk` straight into a git-tracked file — see git history), this
version assembles `/run/wpa_supplicant-wlan0.conf` at boot from a static
base plus `/data/wifi-client.conf`, reusing the exact same
"cat base + secret file" pattern the AP work already built for its own
passphrase (`ultima-wpa-supplicant-config.service`, mirroring
`ultima-hostapd-config.service`) — worth keeping even though AP mode
itself is gone, for the same reason it was built the first time: a home
WiFi password doesn't belong in repo history.

**Hardware-verified end to end (2026-08-19):** rebuilt, reflashed to eMMC,
provisioned `/data/wifi-client.conf` with the Skynet stanza (`psk`
confirmed still current), restarted the `ultima-wpa-supplicant-config` →
`wpa_supplicant@wlan0` chain. `wpa_cli -i wlan0 status` showed
`wpa_state=COMPLETED` against Skynet with a normal DHCP lease, `ultima-app`
stayed active with zero restarts through the WiFi service restarts, and
`systemctl --failed` came back empty. Board is back to its pre-session
client-mode baseline (modulo the `/data` loss noted below).

### Discovered along the way: reflashing eMMC silently wipes `/data`

`emmc-install.sh`'s image write is `dd` of the *entire* disk
(`f.xz` bakes partitions 1+2+3 together, not just boot+rootfs), so every
reflash was silently resetting `/data` to whatever blank partition 3 the
image ships with — not something either eMMC promotion before this one
happened to surface, since neither had anything real on `/data` yet at the
time. Caught live during this revert's own reflash prep: `/data/wifi-ap.conf`
(the AP's provisioned SSID/passphrase) was gone after a routine push, and
`/data/odometer.json` had reset to `2347` — confirmed that's literally
`OdoStore`'s hardcoded `DEFAULT_TOTAL_ODO`, not a coincidence, meaning
whatever real odometer value existed before that flash is gone.

Fixed in `emmc-install.sh` itself, not worked around per-flash: before the
whole-disk `dd`, mount the *old* `/dev/mmcblk0p3` read-only and `tar` it to
`/tmp/data-backup.tar`; after the write (and `blockdev --rereadpt`), mount
the *freshly-flashed* `/dev/mmcblk0p3` (guaranteed a valid empty ext4
filesystem — the `.wic` bakes a real filesystem for `/data` at build time,
not just reserved space) and untar the backup onto it. Best-effort by
design: a first-ever flash (no `/data` filesystem yet) or an unmountable
old `/data` just skips the backup and proceeds with a blank one, rather
than blocking the whole image update on a partition that might already be
in a bad state — this is meant to stop *silent* loss on routine reflashes,
not to be a real backup system.

Layout-independent by construction (tar-based, not partition-offset math)
— considered and rejected a partial-`dd`-that-skips-p3 approach instead:
faster, but only safe as long as the `.wks` partition layout never changes
between flashes, and silently corrupts `/data` at the old offsets the
moment it does. The tar approach can't hit that failure mode.

**Ran for real during this revert's own reflash — hit the safe-fallback
path, not the backup/restore path.** The install log showed "no readable
/data on the current eMMC ... proceeding without a backup" rather than
"backed up /data" — `mount -t ext4 -o ro /dev/mmcblk0p3` failed even though
that partition should have held the just-written `wifi-ap.conf` from
earlier in the session. Root cause unconfirmed: the install log lives on
the SD-booted board's tmpfs and was gone once the script powered the board
off per its own instructions, so the actual mount error was never captured.
No real loss resulted (that `/data` only held throwaway AP test credentials
and the already-defaulted `odometer.json` — the real odometer value was
lost in the *prior* flash, before this fix existed), but **the
backup→restore round-trip itself is still unproven** on this hardware. Next
reflash: write a marker file to `/data` first and confirm both the "backed
up" log line and the marker's survival, to actually exercise the code path
this was written for instead of its fallback.

**Third occurrence, still the fallback path — a later eMMC push
(2026-08-19).** `emmc-push.sh` for a later image again logged "no
readable /data on the current eMMC ... proceeding without a backup," same
as before. Didn't write the marker file this suggested beforehand — wasn't
anticipated going into that push — so the backup→restore round-trip is
*still* unexercised after three real reflashes. No real consequence this
time either: `DEFAULT_TOTAL_ODO` is `0.0` as of the odometer-reset commit
below, which is exactly what a wiped `/data` falls back to anyway, so
there was nothing live to lose. But the marker-file test from the
paragraph above remains not done — still worth doing on the next reflash
whenever there's actually something real on `/data` to protect.

## Odometer reset to 0 (2026-08-19)

Requested directly (not tied to any bug): reset the total odometer to 0,
including the hardcoded fallback, not just the persisted value — so a
future lost/corrupted `/data/odometer.json` (see the eMMC-wipe incident
just above) reverts to 0 instead of reviving `2347.0`. Worth noting the
`2347.0` being replaced was already itself the fallback default, not a
live-tracked real value — per that same incident, the actual real-world
odometer reading was lost in an earlier reflash, before the backup/restore
fix existed. Nothing genuine was discarded here, just an old placeholder.

Changed `DEFAULT_TOTAL_ODO` in `odostore.cpp` from `2347.0` to `0.0`, hot-
deployed via the loop above, and reset the live value using the safe
sequence "Odometer-persistence correctness test" documents further up this
file: `systemctl stop ultima-app` *before* touching `/data/odometer.json`,
not while it's running — otherwise the app's own SIGTERM-triggered save
overwrites a live edit with its stale in-memory value on the way down.
Wrote `{"totalOdo":0,"tripOdo":0}`, swapped the binary, `systemctl start`,
confirmed via journal: `OdoStore: saved totalOdo=0.0 tripOdo=0.0` on the
next 30s autosave, file contents matching.

## CAN interface migrated from USB adapter to mikroBUS SPI click (2026-08-20) — DT/kernel path hardware-verified 2026-08-21; click board seated and SPI-verified 2026-08-26, CAN2 wiring still pending

Replaced the ODrive USB-CAN adapter with a MikroE **CAN SPI 3.3V click**
(MCP2515 CAN controller + SN65HVD230 transceiver) plugged into BeaglePlay's
mikroBUS header, wired to the same Syvecs S7+ CAN2 bus. Motivation: get the
dash off a USB dongle. This is a straight replacement, not an addition —
`can0` is now the SPI device; nothing in `canbus.cpp` changed (it was already
generic `PF_CAN`/`SOCK_RAW` against whatever `can0` is).

**Falcon boot forces this to be a compile-time DT change, not a runtime
overlay.** BeaglePlay's normal path (and the `k3-am625-beagleplay-release-
mikrobus.dtbo`-style overlays BeagleBoard docs describe) applies `.dtbo`
overlays inside U-Boot proper via `extlinux.conf`'s `fdtoverlays`. Falcon
boot skips U-Boot proper entirely (R5 SPL → ATF → OP-TEE → Linux directly,
see "Falcon boot" above) — there's no stage in this project's boot path that
ever runs `fdtoverlays`. Confirmed nothing in either custom layer sets
`KERNEL_DEVICETREE` or references a project-local `.dts`; this project has
always shipped `meta-ti-bsp`'s stock upstream `k3-am625-beagleplay.dts`
unmodified until now. So the MCP2515 node has to be baked into the dtb at
kernel build time, same pattern as the existing U-Boot falcon-boot binman
dtsi patch (`meta-falcon-beagleplay-src/recipes-bsp/u-boot/u-boot-ti-staging/
0001-arm-dts-k3-am625-beagleplay-add-falcon-boot-binman-.patch`).

**New patch**:
`meta-ultima-beagleplay-src/recipes-kernel/linux/linux-ti-staging/
0001-arm64-dts-k3-am625-beagleplay-add-mikrobus-can-click.patch`, wired into
`linux-ti-staging_%.bbappend`'s `SRC_URI:append` (plain unified diff, not
git-style — same quilt gotcha as the U-Boot patch, see "Must be a plain
unified diff" above). Appends (doesn't edit in place) two label-referenced
blocks at EOF: a `fixed-clock` node for the MCP2515's crystal, and a
`can@0` child node under `&main_spi2`. Appending rather than patching the
existing `&main_spi2 { ... }` block in place was deliberate — DTS allows
re-opening an already-labeled node anywhere later in the file to add
children, so the patch only needs a few lines of tail context to apply
instead of matching deep into upstream's file, and survives minor upstream
dts drift.

Pin/hardware facts, pinned down rather than guessed (each was a real
research step, not assumed):
- **BeaglePlay has exactly one mikroBUS site** (confirmed via BeagleBoard
  docs/forum search) — no "which socket" ambiguity.
- **mikroBUS SPI is already stock-enabled**: `&main_spi2` ships
  `status = "okay"` with `mikrobus_spi_pins_default` (CLK/CS0/D0/D1) in the
  base dts already — just had no child device. CS is hardware SPI2_CS0
  (`reg = <0>`), no manual GPIO chip-select needed.
- **INT → `main_gpio1` line 9.** The stock dts's `gpio-line-names` labels
  three plain-GPIO mikroBUS pins generically (`MIKROBUS_GPIO1_9/10/12`) —
  it does *not* say which is INT/AN/RST by name. Cross-referenced against a
  BeagleBoard forum post that mapped `gpioinfo` output to mikroBUS signal
  names (GPIO1_9=INT, GPIO1_10=AN, GPIO1_12=RST) and confirmed internally
  consistent: mikroBUS PWM has its own dedicated pin (`ecap2`/
  `mikrobus_pwm_pins_default`, a different physical net entirely), which is
  exactly why the plain-GPIO group has 3 members (INT/AN/RST), not 4.
  `IRQ_TYPE_LEVEL_LOW` — MCP2515's `/INT` is a level output, held low until
  the driver clears the interrupt source, not edge.
- **RST needs no DT property at all.** Pulled the actual MikroE schematic
  (`can-spi-click-33v-manual-v100.pdf`): the click's `/RESET` pin has its own
  onboard 100 kΩ pull-up to VCC, and the mainline `microchip,mcp251x`
  binding has no `reset-gpios` property in the first place (driver
  soft-resets the chip over SPI on probe). Wiring mikroBUS RST into the DT
  node would have been actively wrong, not just unnecessary.
- **10 MHz crystal** (X1 on the click's schematic) → `clock-frequency =
  <10000000>` on the new `fixed-clock` node, referenced by the MCP2515's
  `clocks` property, plus `spi-max-frequency = <10000000>` (MCP2515's SPI
  ceiling). This is the fact most likely to silently break CAN if wrong —
  SocketCAN's bit-timing calculator derives BRP/PROP_SEG/PS1/PS2 from this
  value, and a wrong oscillator produces exactly the "bit-timing not yet
  defined" error a BeagleBoard forum user hit trying this same chip on
  BeaglePlay's mikroBUS. Pulled from the schematic, not remembered/assumed.
- Click's termination jumper (J2) takes over the role the ODrive adapter's
  switchable 120 Ω terminator played — populate it, same reasoning as
  before (Syvecs CAN2 has no on-board termination of its own).

**Verified locally before touching the real build** (kernel source isn't
checked into this repo — lives only in the `falcon-yocto-build` docker
volume — so this was done against a reference copy): fetched
`k3-am625-beagleplay.dts` and its full include chain from BeagleBoard's
`BeagleBoard-DeviceTrees` GitHub repo at the `v6.12.x` branch (matches this
project's `linux-ti-staging` 6.12.57 exactly), applied the patch
(`patch -p1`, clean), preprocessed with `clang -E -x assembler-with-cpp`,
compiled with `dtc` (Homebrew) — no errors or warnings — then decompiled the
resulting `.dtb` and confirmed `can@0` landed as an actual child of the real
`main_spi2` node with `clocks`/`interrupt-parent` phandles resolving to the
new fixed-clock and `main_gpio1` respectively, and `interrupts = <9 8>` (8 =
`IRQ_TYPE_LEVEL_LOW`). This de-risks the DTS syntax/semantics but is not a
substitute for a real build + boot — TI's actual `linux-ti-staging` tree
could differ from BeagleBoard's upstream in ways this reference copy
wouldn't catch.

**Kernel config** (`ultima-can.cfg`, same file, `CONFIG_CAN_GS_USB` removed):
`CONFIG_CAN_MCP251X=y`, `CONFIG_SPI=y`, `CONFIG_SPI_OMAP24XX=y`, all built-in
rather than modules. Deliberate, not just "why not `=y`": this is a
DT-instantiated platform/SPI device rather than a hotplugged USB one, and
DT-platform-device module coldplug already burned this project once
(`CONFIG_DRM_TIDSS` — see that section above, never root-caused why coldplug
didn't fire, fixed by forcing the module via `/etc/modules-load.d/`
instead). Building the mcp251x driver in sidesteps that whole class of bug
rather than re-litigating it.

**udev rule** (`70-can.rules`, same file, content replaced): was
`SUBSYSTEM=="net", ACTION=="add", DEVPATH=="*/usb*", ATTRS{idVendor}=="1d50",
ATTRS{idProduct}=="606f"`, now `SUBSYSTEM=="net", ACTION=="add",
SUBSYSTEMS=="spi", DRIVERS=="mcp251x"` — matches the netdev's parent SPI
device by driver name instead of a USB VID/PID, same `ip link set ... type
can bitrate 1000000` + `up` `RUN+=` actions. Also dropped the comment
pointing at `br2-external/board/ultima-beagleplay/overlay/etc/udev/
rules.d/70-can.rules` as the "keep in sync" source — that whole Buildroot/
RPi5 tree doesn't exist in this repo anymore (see CLAUDE.md), so the pointer
was already stale independent of this change.

**Not yet done (as first written above)**: no board access that session, so
nothing had touched real hardware yet — needs `beagleplay-falcon/build.sh`,
a flash, and a `candump`/`ip -details link show can0` check with the click
actually wired to the ECU's CAN2 (B2/CAN_H, B3/CAN_L) before trusting any of
this end to end. The DTS verification above only proved the tree compiles
and merges correctly, not that the physical INT/CS/oscillator wiring is
right on real silicon.

### Hardware verification (2026-08-21) — DT/kernel path confirmed, click board not yet connected

Built (`beagleplay-falcon/build.sh`), flashed to SD (`flash.sh disk4`), and
booted with USR held. Confirmed over SSH:

- `dmesg`: `mcp251x spi0.0: MCP251x didn't enter in conf mode after reset` /
  `probe with driver mcp251x failed with error -110` (`-ETIMEDOUT`).
- `/sys/bus/spi/devices/spi0.0` exists — the DT node instantiated a real SPI
  child device. (Linux numbers SPI *master* controllers by probe order, not
  by DT label suffix — `main_spi2` in the dts becomes `spi0` here since
  it's the only SPI controller enabled on this board; that's expected, not
  a bug.)
- `uname -r` reported `6.12.57-ti-01316-g31b07ab8dfbc-dirty` (the `-dirty`
  suffix is `scripts/setlocalversion` correctly flagging the kernel patch
  applied via `0001-arm64-dts-...-add-mikrobus-can-click.patch`) — confirms
  this was genuinely today's build, not a stale cached one.

**This is exactly the outcome you'd expect with an empty mikroBUS socket,
not a DT/config bug**: the driver got far enough to issue a real SPI reset
transaction and time out waiting for a response, which only happens if the
kernel-side plumbing (pinmux, SPI bus routing, chip-select, IRQ GPIO,
`CONFIG_CAN_MCP251X` actually built) is already correct — an unpowered/
disconnected chip can't do anything else. Confirmed with the user: the
click board is not physically plugged into the mikroBUS socket yet. So this
fully verifies the compile-time/DT/kernel-config side of the CAN-SPI work;
the only remaining unknown is the physical click board itself (INT/RST/CS
pin assignment against real silicon, oscillator behavior, CAN2 wiring to
the ECU) — untestable until the board is seated and wired.

**One real debugging trap hit and resolved along the way, unrelated to the
CAN-SPI work itself**: the first post-flash SSH check showed the *old*
gs_usb-era `70-can.rules` and a kernel `.config` with none of this
project's fragment values — looked exactly like a DT/Kconfig bug, but
turned out to mean the SD card hadn't actually been reflashed with the
just-built image (checked `cat /etc/udev/rules.d/70-can.rules`: old
USB-VID/PID rule ⇒ stale rootfs; new `SUBSYSTEMS=="spi"` rule ⇒ genuinely
fresh boot — a two-minute check that's cheaper than re-deriving the whole
DT/Kconfig chain from scratch next time this symptom shows up). `flash.sh`
stamping every card it writes with the same `deadbee5` PARTUUID (see
"Flashing" above) means that signature alone doesn't prove *today's* flash
took — only that *some* `flash.sh` run wrote that card at some point.

### Hardware verification (2026-08-26) — click board seated, SPI/chip init confirmed, CAN2 wiring still pending

User physically seated the MikroE CAN SPI 3.3V click into BeaglePlay's
mikroBUS socket (no CAN2 wiring to the ECU yet — just the click board
itself, powered from the mikroBUS rail). First check against the board's
then-current eMMC image showed nothing at all: no `/sys/bus/spi/devices/`
entries, no `can0`, no `mcp251x` in `dmesg`. Not a wiring problem — the
running eMMC image simply predated this project's history ever adding the
DT patch to a build that got flashed. (Blind alley worth noting: the
kernel's `Linux version` banner date — `Thu Dec 4 13:07:37 UTC 2025` — is
*not* a useful staleness signal here. OE's reproducible-build timestamping
pins it regardless of when `bitbake` actually runs, confirmed by grepping
strings out of a freshly built `.wic.xz` that already contained
`microchip,mcp2515`/`mikrobus_can` and reported the exact same banner date.
The only real signal is checking the DT/sysfs/dmesg state directly.)

Rebuilt and reflashed to get a current image onto eMMC:

- Working tree had unrelated uncommitted WIP (a dashcam-recording
  libjpeg-turbo encode spike touching `camerafeed.cpp`/`ultima-app.pro`/
  `ultima-app.bb`) — `build.sh` bind-mounts the live source tree, so this
  would have been baked into the image too. Stashed it for the duration of
  the build, popped it back after — the built image reflects clean `HEAD`
  only (CAN patch + that morning's camera-fps-fix and SetTimeScreen
  commits), not the in-progress spike.
- `build.sh` (incremental — 8988/9030 tasks reused from sstate, only the
  kernel/rootfs actually rebuilt) produced a `.wic.xz` confirmed via string
  search to contain the CAN DT node.
- `deploy-falcon/tiboot3-falcon-emmc.bin` didn't exist yet at all — this is
  the first time anything actually pushed an image to this board's eMMC
  since the click was added to the DT, so `build-emmc-spl.sh` had to be run
  for the first time, not just re-run.
- `flash.sh` to SD (run by the user directly — see "Must be run
  interactively" under "Flashing" above), booted with USR held, confirmed
  root on `/dev/mmcblk1p2` before touching anything.
- `emmc-push.sh` over SSH from the SD-booted board installed the new image
  + eMMC SPL onto `/dev/mmcblk0`. Its pre-install backup step logged "no
  readable /data on the current eMMC (first flash, or partition not yet
  formatted) — proceeding without a backup"; whatever was in `/data` before
  (e.g. a provisioned `/data/wifi-client.conf`, if this board had one) did
  not survive and would need re-provisioning. Disk signatures came out
  distinct as designed (`emmc=2a4a6c07` vs the SD's `deadbee5`).
- Powered off, pulled the SD card, powered back on with no button held.
  Confirmed root on `/dev/mmcblk0p2`, hostname `ultimagc` (this image also
  carries the 2026-08-25 hostname-rename commit).

**Result — real progress over the 2026-08-21 check, not just a repeat**:

```
mcp251x spi0.0 can0: MCP2515 successfully initialized.
```

`/sys/bus/spi/devices/` now shows `spi0.0`, `can0` exists as a netdev
(`DOWN`), and `lsmod` shows `mcp251x`/`can_dev` loaded. This is qualitatively
different from the 2026-08-21 result (`-110`/`ETIMEDOUT`, empty-socket
behavior) — the chip is seated correctly, powered, and actually answering
SPI transactions now. The two lines right after —

```
mcp251x spi0.0 can0: bit-timing not yet defined
mcp251x spi0.0: unable to set initial baudrate!
```

— are expected/benign, not a fault: SocketCAN logs these until a bitrate is
set (`ip link set can0 type can bitrate <rate>`) and the interface brought
up, and nothing is wired to CAN2 yet for there to be a bitrate to derive.

**Still not done**: physical CAN2 wiring to the Syvecs S7+ (B2/CAN_H,
B3/CAN_L), populating the click's termination jumper (J2), setting the
correct bitrate, and a real `candump`/`ip -details link show can0` check —
none of the oscillator/bit-timing math from the original DT work has been
exercised against real bus traffic yet.

## Two environmental build failures, unrelated to any feature work (2026-08-21)

Hit while building the CAN-SPI change above — both pre-existing, both would
strike on *any* `build.sh` run in this specific checkout, not something the
CAN change caused. Fixed since they blocked getting a bootable image at
all.

**mycam004m sibling repo resolves to nothing on this checkout.**
`build.sh`'s `MYCAM004M_SRC="$(cd ../../mycam004m ...)"` assumes UltimaGC is
checked out under `~/code/` alongside its sibling repos (the comment in
`mycam004m.bb` says as much: "~/code/mycam004m"). This checkout lives on a
mounted network share (`/Volumes/UltimaBuildCode/UltimaGC`) instead, so the
relative path resolves to nothing, silently falls back to an empty stub
dir, and `mycam004m.bb`'s `do_populate_lic` fails on a missing `LICENSE`
file — a confusing failure mode for what's really just "wrong path,"
since IMAGE_INSTALL forces mycam004m into every `tisdk-base-image` build
(same "can't silently drop out" pattern as ti-img-rogue-driver). Confirmed
the real repo exists at `/Users/jellis/code/mycam004m` with the exact
`LICENSE` md5 the recipe wants (`eb723b61539feef013de476e68b5c50a`), and
that OrbStack already shares `/Users` into the build container. Fix:
`build.sh` now also tries `~/code/mycam004m` before falling back to the
empty stub. Three separate assignments, not one `A && B || C && D || E`
one-liner — chaining a second `&& pwd` fallback after an `||` hits real
`&&`/`||` precedence and `set -e` traps (tested empirically, not assumed):
the first `pwd` gets *re-run* a second time whenever the first `cd` already
succeeded (silently duplicating the path with a newline in between), and
under `set -e` a failed `cd` inside a `$(...)` assignment aborts the whole
script right there rather than falling through to the next candidate —
`|| true` after each `cd ... && pwd` is required to neutralize that.

**`ultima-app`'s SMB mount has stale, lock-stuck `.smbdeleteAAA*`
files** that broke `ultima-app.bb`'s `do_unpack` `shutil.copytree`
(`shutil.Error: [Errno 2] No such file or directory` on files that were
present moments earlier during `scandir`). These are macOS smbfs's
rename-based delete-on-network-share artifacts, not real source — one
checked directly turned out to be a stale pre-edit snapshot of
`tisdk-base-image.bbappend` still containing the entire Bluetooth feature
this repo's most recent commit (`fe8b445`) removed. Can't just delete them:
every `rm` returned `Resource busy` (a real OS-level SMB lock, not a
permissions problem — `sudo rm` won't help, and unmounting/remounting the
share to clear it is a non-starter since the repo itself lives on that
share). Fix instead: `ultima-app.bb`'s `do_unpack:append()` now passes
`ignore=shutil.ignore_patterns('.smbdelete*')` to `copytree`, filtering them
at the `scandir` step regardless of their lock state — permanent against
this SMB quirk rather than racing a retry and hoping the ghost files happen
to not collide with the copy window that time. Left the ghost files
themselves in place (this is the user's SMB workflow producing them, not
something to clear from here); a build hitting a *new* location's ghost
files would need the same `ignore=` treatment there, not another retry.

## mycam004m real backend wired up (2026-08-23) — DT patch built and dtc-verified, not yet flashed/hardware-tested

MY-CAM004M hardware is now physically attached (`~/code/mycam004m` is the
driver source of truth — register tables, device contract, bring-up docs;
see its README and `docs/testing.md`/`docs/ultima-app-integration.md`).
This session wired the *real* backend into the Yocto build and made it the
boot default. `ultima-app` needed zero changes — `camerafeed.cpp`/
`main.cpp` already target `/dev/mycam/cam1..4` at 1920x1080 (from the
earlier fake-backend integration, see git log `daec7a9`) and the driver
repo's device contract is backend-agnostic by design.

**Same Falcon-boot constraint as the mikroBUS CAN click above:** the
driver repo's own `dts/k3-am625-beagleplay-mycam004m.dtso` is a *runtime*
overlay (applied via U-Boot's `fdtoverlays`), which this project's boot
path never runs. Converted it to a compile-time patch instead —
`meta-ultima-beagleplay-src/recipes-kernel/linux/linux-ti-staging/
0002-arm64-dts-k3-am625-beagleplay-add-mycam004m-camera.patch`, wired into
`linux-ti-staging_%.bbappend`'s `SRC_URI:append` alongside the existing
CAN patch (applies second, appends further down the same file).

**Real physical wiring diverges from the driver repo's overlay: no
reset-gpios/pwren-gpios.** The user's own pin-mapping doc
(`docs/MY-CAM to BeaglePlay Pin Mapping.pdf` in the main UltimaGC repo, not
this one) shows the actual cable carries only the 4 CSI-2 data lanes +
clock lane + I2C (SCL/SDA) — BeaglePlay's two J17 GPIOs
(`CSI2_CAMERA_GPIO1`/`GPIO2`, `main_gpio0` pins 11/12) and the camera's
own `RST_N`/`PWRDN`/`MCLK` are all struck through in that doc, i.e. not
wired at all. (MCLK not being wired checks out independently too — the
MY-CAM004M schematic, `~/code/mycam004m/MY-CAM004M/my-cam004m-20230725.pdf`,
shows an onboard crystal oscillator, `Y1`, feeding the N4 decoder's
`SYS_CLK` directly, so it never needed an external MCLK from BeaglePlay.)
The new DT patch's camera node omits `reset-gpios`/`pwren-gpios` entirely
rather than pointing them at GPIOs the cable doesn't carry —
`mycam004m.c` requests both as fully optional
(`devm_gpiod_get_optional`), so this is a clean no-op for the driver, not
a workaround. **The one real open question this doesn't resolve**:
whether the N4's `RSTB` pin has an onboard pull (so leaving it
undriven is fine) or floats (so the chip may never leave reset,
which would show up as an I2C probe failure). Not resolved by reading the
schematic — flattened/OCR'd schematic text isn't reliable for tracing
pull-resistor connectivity, and guessing here risks chasing a phantom
wiring problem instead of a real one. The correct arbiter is empirical,
not more schematic-reading: `i2cdetect -y 4` for `0x30` and
`dmesg | grep mycam004m` for `DEV_ID 0xb0` after flashing — see "Not yet
done" below. A DT change can't fix a floating reset either way; only a
physical wire or a populated onboard pull can, so this doesn't block
shipping the DT/recipe side of the work.

**DT patch verified the same way the CAN patch was, but against this
project's *actual* kernel source this time** (not a downloaded reference
copy — the `falcon-yocto-build` Docker volume already has the real,
already-patched `k3-am625-beagleplay.dts` and the matching `cpp`/`dtc`
toolchain on disk, so there was no need to approximate against upstream).
Applied the new patch, preprocessed with `aarch64-oe-linux-cpp` (adding
`-I .../arch/arm64/boot/dts/ti` beyond what the driver repo's README
needed, since compiling the *whole* board dts — not a standalone overlay —
pulls in `#include "k3-am625.dtsi"` and friends), compiled clean with the
kernel's own `scripts/dtc/dtc` (exit 0), then decompiled the `.dtb` back
to source and checked byte-for-byte: `camera@30`'s `reg = <0x30>` and
`link-frequencies` decoding to exactly `1242000000`, `mycam004m_out`'s
`remote-endpoint` phandle resolving to `csi2rx0_in_mycam004m` and back
(genuine bidirectional graph link, not just two nodes that happen to
compile), `port@0`/`dphy0`/`ti_csi2rx0` all landing `status = "okay"`.

One correction to the driver repo's own overlay in the process: reopened
the *existing* labeled `port@0` (`cdns_csi2rx0`'s SoC-dtsi-level node is
already `csi0_port0`, one of five pre-declared ports) via `&csi0_port0`
instead of re-declaring a fresh `ports { #address-cells ... port@0 { ... } }`
wrapper — the overlay had to declare that structure itself (overlay merge
matches by node path, not by label), but a same-file compile-time patch
can and should just reopen the label directly, same philosophy as the CAN
patch reopening `&main_spi2` rather than rewriting it. Decompiled output
confirms both approaches land identically either way.

**Kernel config: no fragment needed, confirmed rather than assumed.**
Grepped the actual built `.config` in the Yocto volume (not the fragment,
matching the lesson from the `CONFIG_DRM_TIDSS` incident above) for the
CSI2/D-PHY chain: `CONFIG_VIDEO_CADENCE_CSI2RX=m`,
`CONFIG_VIDEO_TI_J721E_CSI2RX=m`, `CONFIG_PHY_CADENCE_DPHY_RX=m`,
`CONFIG_MEDIA_CONTROLLER=y`, `CONFIG_V4L2_FWNODE=m`/`CONFIG_V4L2_ASYNC=m`
all already present in arago's base defconfig — nothing to add. Deliberately
did **not** try forcing any of these `=y` the way `ultima-can.cfg` did for
`CONFIG_CAN_MCP251X` — `CONFIG_MEDIA_SUPPORT=m` in this same `.config`
means a `=y` request for anything depending on it would hit the exact same
silent-downgrade trap already root-caused for `CONFIG_DRM_TIDSS`
(`CONFIG_DRM=m` capping `CONFIG_DRM_TIDSS=y` requests) — a tristate can't
be built in while its parent is modular. Real fix, matching the fix
already in place for `tidss`/`mcp251x`: force explicit module load via
`/etc/modules-load.d/` instead of trusting coldplug for a DT-instantiated
device (`meta-ultima-beagleplay-src/recipes-kernel/mycam004m/files/
mycam004m-real.conf`: `j721e-csi2rx`, `cdns-dphy-rx`, `mycam004m` —
`cdns-csi2rx` and the v4l2 core modules pull in automatically via
`modprobe`'s own dependency resolution, confirmed via each `.ko`'s
`modinfo depends=`).

**Packaging gap that would have silently left the real backend
non-functional even with the DT/Kconfig side correct**: this image's
`IMAGE_INSTALL` is a curated, trimmed list (see `tisdk-base-image.bbappend`
and the read-only-rootfs/boot-trim work), not a generic "kernel-modules"
bundle — `kernel-module-split.bbclass` packages every built `.ko`
regardless, but nothing pulls `kernel-module-j721e-csi2rx`/
`kernel-module-cdns-dphy-rx` onto the target rootfs unless something
`RDEPENDS` on them. Added `RDEPENDS:${PN} += "kernel-module-j721e-csi2rx
kernel-module-cdns-dphy-rx"` to `mycam004m.bb` (`kernel-module-cdns-csi2rx`
comes along transitively, same auto-RDEPENDS mechanism module.bbclass
already uses for `mycam004m`'s own videodev/mc/etc. dependencies).

**`select-camera-backend.service` now defaults to `real`** (was `fake`).
Judged safe to default rather than requiring a manual post-flash SSH step:
if the camera never probes (the RSTB question above), `mycam004m`'s async
notifier never completes and no `/dev/videoN` context nodes get created
for *any* of the 4 streams — `select-camera-backend.sh` finds 0 matching
nodes, logs a warning, and exits 1 without touching `/dev/mycam/cam1..4`
(the script's own `rm -f "$LINKDIR"/cam[1-4]` at the top means the symlinks
just don't exist rather than pointing at something broken) — a clean,
diagnosable failure, not a hang or a silent bad-frame stream. `mycam004m-fake`
stays built and force-loaded alongside it either way, so
`ssh root@beagleplay-ti.local select-camera-backend.sh fake` is a one-line
fallback if `real` doesn't come up clean on first boot.

### First real hardware boot (2026-08-23, same session) — CSI2/D-PHY side confirmed working, wrong I2C address found and fixed

Built (`build.sh`), pulled the image (the `docker cp`-equivalent step above
— skipping it would silently reflash a stale image, see "Two forgotten
steps" further up this file), flashed to SD (`flash.sh disk4`), booted
with USR held. `uname -r` confirmed `6.12.57-ti-01316-g31b07ab8dfbc-dirty`
— genuinely today's build, not stale.

**The CSI2/D-PHY/kernel-module side of this work is fully verified
correct**, not just dtc-clean: `dmesg` shows `cdns-csi2rx 30101000.csi-bridge:
Probed CSI2RX with 4/4 lanes, 4 streams, external D-PHY` — the bridge
itself came up clean on real hardware. `lsmod` confirms every module from
`mycam004m-real.conf` force-loaded correctly (`mycam004m`, `cdns_dphy_rx`,
`j721e_csi2rx`, `cdns_csi2rx`, plus the auto-pulled v4l2 core modules) —
the modules-load.d fix and the `RDEPENDS` packaging fix (the modules
actually being *on* the rootfs) both did their job. The
"Fixed dependency cycle(s)" lines in `dmesg` are normal fwnode-graph
resolution chatter for a bidirectional endpoint link, not an error.

**The camera itself didn't probe — but not for the reason anticipated.**
`mycam004m 4-0030: error -EREMOTEIO: failed selecting bank 0x00 for
chip-ID readback` / `probe with driver mycam004m failed with error -121`.
This actually *resolves* the RSTB-floating question flagged above as the
one real unknown: `error -EREMOTEIO` (a real I2C bus rejection, not a
timeout/hang) is only reachable if the chip already came out of reset
enough to sit on the bus — a permanently-in-reset N4 wouldn't NAK a
specific bank-select write, it just wouldn't be there at all. `i2cdetect
-y 4` confirmed exactly that shape: nothing at `0x30`, but a real device
answering at **`0x31`**. The driver repo's own overlay/README (and this
project's first cut of the DT patch, above) used `0x30`, sourced from the
MY-CAM004M schematic's "I2C add 0x61" label and N4's own
SA0/SA1-strap address formula — hardware disagrees with that documentation
on this actual board, whatever the real strap turns out to be. Fixed by
changing the DT patch's camera node from `camera@30`/`reg = <0x30>` to
`camera@31`/`reg = <0x31>` (regenerated cleanly from the pristine base
file rather than `sed`-ing the applied one in place — a first attempt at
a quick in-place `sed 's/reg = <0x30>/reg = <0x31>/'` silently also
rewrote the unrelated `tps65219` PMIC node's own legitimate `reg = <0x30>`
elsewhere in the same file; caught by grepping the result before reusing
it, not by luck). Re-verified with the same `cpp`+`dtc` compile/decompile
check as before — `camera@31`/`reg = <0x31>` lands correctly and
`pmic@30`/`reg = <0x30>` is untouched.

Not yet re-flashed/re-tested with the corrected address — that's the
immediate next step, same verification sequence: `dmesg | grep
mycam004m` for `DEV_ID 0xb0`, `media-ctl -d /dev/media0 -p` for the
4-entity graph, then a real streaming test per `~/code/mycam004m/docs/
testing.md`. Worth feeding this address correction back into
`~/code/mycam004m` itself (the driver repo, separate git repo/source of
truth) once confirmed working, since its README's Status table and the
`dts/k3-am625-beagleplay-mycam004m.dtso` overlay both still say `0x30`.

### Media pipeline bring-up + hardware signal-lock check (2026-08-23, same session) — VIDIOC_STREAMON fixed; remaining blocker is upstream of the SoC entirely

With the I2C address fixed, `VIDIOC_STREAMON` on `/dev/mycam/cam1` still
failed: `Broken pipe` (EPIPE). Root cause, found live over SSH with a
`media-ctl` binary + `libmediactl.so.0`/`libv4l2subdev.so.0` copied to
`/tmp` from this session's own Yocto build output (no rebuild needed to
iterate) — `media-ctl -d /dev/media0 -p` showed `mycam004m`'s own pads
correctly self-initialized to `YUYV8_1X16/1920x1080`, but
`cdns_csi2rx...csi-bridge` and `...ticsi2rx` both sat on an unrelated
uninitialized default (`UYVY8_1X16/640x480`) that never auto-propagated
from the camera's pads — a real, unconfigured media-controller pipeline,
not a driver bug. Setting the active format explicitly on both
(`media-ctl -V "\"cdns_csi2rx...csi-bridge\":0 [fmt:YUYV8_1X16/1920x1080]"`,
same for `"...ticsi2rx":0`) fixed it immediately — confirmed via
`journalctl -u ultima-app`: `[camerafeed] streaming /dev/mycam/cam1:
960x540 YUYV`, sustained (no drop) for 25+ seconds on a fresh post-reboot
boot with **no route changes**, just the two format calls. This is now
baked in permanently as `mycam004m-configure-pipeline.service` (a oneshot,
`After=systemd-modules-load.service`, same pattern as
`select-camera-backend.service`) + `media-ctl` added to `mycam004m.bb`'s
`RDEPENDS` (the split, libv4l/Qt-free package — this minimal rootfs had
no v4l-utils at all before this).

**Scoped to stream 0 only, deliberately — routes for streams 1-3 were
tried and are NOT baked in.** Only one physical camera is attached to the
board as of this writing. A first pass also added the 3 additional
`media-ctl -R` routes (`cdns_csi2rx`'s `0/1->1/1` etc., `ticsi2rx`'s
`0/1->2/0` etc.) so all 4 `/dev/mycam/camN` would resolve — this visibly
changed behavior (`dmesg` went from only ever logging `enabling AHD input
0` to logging inputs 0-3 together) but was never verified *not* to
regress the one input that matters, because the investigation moved on to
the signal-lock check below before that could be confirmed. Concretely:
cam2-4's already-expected-to-fail `STREAMON` attempts touch more of the
shared bridge/decoder state with 4 routes active than with the default
single route, where they fail early (no route → immediate `ENODEV`)
without reaching it. Revisit once more cameras are physically connected:
re-add the routes, and specifically re-confirm input 0 still streams
cleanly with all 4 active before keeping the change — don't just assume
it composes.

**`VIDIOC_STREAMON` succeeding is not the same as a real picture — caught
by an on-device screenshot, not a log line.** `main.cpp`'s existing debug
hooks (`/tmp/ultima-camtest.request` = `open` opens `CameraGridScreen`,
`/tmp/ultima-screenshot.request` grabs a frame to
`/tmp/ultima-screenshot.png`, both already in `ultima-app`, no rebuild
needed) showed all 4 quadrants reading "NO SIGNAL" even immediately after
confirming cam1's sustained sillicon-level stream via `journalctl`/
`dmesg`. `CameraGridScreen.qml`'s label ties directly to `CameraFeed`'s
`streaming` property, which per `camerafeed.h`'s own comment only goes
true once a first real frame has actually been read back (`onReadable()`
firing) — not once `VIDIOC_STREAMON` returns success. So "the ioctl
succeeded" and "a frame arrived" are genuinely different claims, and only
the second one is the actual acceptance test for this whole feature.

**Discriminator: read the N4 decoder's own signal-lock status registers
directly over I2C** (Bank 0, `NOVID_1..4` at `0xA4-0xA7`, `H_LOCK_1..4` at
`0xD8-0xDB`, `AGC_LOCK_1..4` at `0xD0-0xD3` — all documented in
`Datasheet-N4.pdf`, "Registers to Show Locking Status"; bank-select
mechanism, register `0xff` written with the bank number as the value,
confirmed against `mycam004m.h`'s own `MYCAM004M_REG_BANK_SEL`/
`mycam004m.c`'s bank-switch logic, i.e. read with the exact same
convention the driver itself already uses successfully for `DEV_ID`).
`i2cget`/`i2cset` need `-f` (force) since the kernel driver already has
the I2C client claimed (`Device or resource busy` without it). Result,
sanity-checked against `DEV_ID` reading back `0xb0` correctly in the same
sweep: **all 4 channels — `NOVID_1..4 = 0x01` (no video), `H_LOCK_1..4 =
0x00` (not locked), `AGC_LOCK_1..4 = 0x00` (not locked).** This is the N4
chip's own analog-frontend status, read directly, independent of
`mycam004m.c`/the CSI-2/MIPI/media-graph stack entirely — it cleanly rules
out everything upstream-of-this-point in this session's own work (I2C
address, DT, kernel modules, CSI2 bridge, media pipeline formats/routes,
MIPI PLL/lane-rate table) as the remaining cause. The decoder itself
never sees a valid signal on *any* input, including whichever one the one
physical camera is on.

**Most likely explanation, not yet confirmed**: the camera itself has no
power. The one camera currently attached is a "QJD-SONY 225"
(`索尼225摄像头规格书.pdf` in `~/code/mycam004m/MY-CAM004M/`) — 1/3" Sony
2MP CMOS, AHD 1080p, and per its own spec sheet a **3-wire module: GND,
VIDEO, and a separate DC power input (3.5-6.5V, 50mA)**, visibly a
distinct wire in the spec sheet's own product photo, not something
carried on the video/coax wire. Confirmed with the user: the MY-CAM004M
*board's* own separate 5V/GND lead (the one this session's own DT-mapping
investigation established falls outside BeaglePlay's J17 ribbon — see
above) is connected, but there's no separate power lead run to the
camera's own DC wire. A board that's fully powered and I2C-alive (matches
everything confirmed working this session) while the one physically-wired
camera has no power of its own reaching its sensor produces exactly this
symptom: zero signal, uniformly, no matter which input it's plugged into.
**Not yet physically confirmed** — the next step is wiring the camera's
DC lead to a 3.5-6.5V supply and re-reading the same lock-status
registers live (cheap, no rebuild) to check for a real transition to
`NOVID=0`/`H_LOCK=1`.

**State to leave this in for next session**: the DT/kernel/I2C-address/
media-pipeline work above is real, hardware-verified, and done — don't
re-derive or doubt it without new evidence. What's unverified is purely
downstream of this repo: whether the one attached camera actually has
power. If the lock-status check comes back positive after fixing that,
the natural next check is the same debug-hook screenshot trick above
(confirm `CAM 1` actually shows a picture, not just `H_LOCK=1`) — then,
only once 3 more cameras are physically attached, revisit the 4-route
pipeline change flagged above as not yet safe to bake in.


## mycam004m MIPI output fixed — link-freq 2x error + missing PLL latch/arbiter (2026-08-23, hardware-verified end-to-end)

Follow-up to the entry above, same day. The "most likely explanation:
camera power" hypothesis there is **dead — refuted twice over**: the user
confirmed with a multimeter that 5V does reach the camera connector, and
the MY-CAM product manual (`MY-CAM004M-20240207-产品手册-V1.0.pdf` §3.2.1)
shows each camera socket (CJ-BM4-M11 4-pin: pin1 GND, pin2 VIDEO in,
pin3 VDD_CAM_5V out, pin4 NC) feeds the camera's DC wire from the board's
own 5V rail through the same plug — no separate camera power lead exists
or is needed. The AHD_MD 30fps→25fps register change from the previous
entry also did **not** produce lock (built, flashed, verified active on
real hardware — `NOVID` stayed 1 on all channels), and neither did a
live-I2C sweep of every AHD_MD mode (1080p25/30, 720p25/30/50/60, SD),
nor a full replay of the vendor NVP6324 driver's ~180-register-per-channel
AHD bring-up (AFE banks 5-8, format, EQ stage 0, coax — extracted from
`MY-CAM004M/MYD-LT527/bsp/.../nvp6324/` by an agent, replayed over
`i2cset -f`, values read back and confirmed landed).

What DID come out of this session — the entire **digital output side was
broken, is now fixed, and is verified down to the app's camera view**:

1. **Link frequency was 2x too high — the root cause of a
   zero-frames-after-STREAMON hang.** V4L2's `link-frequencies` /
   `V4L2_CID_LINK_FREQ` is the D-PHY *symbol clock*, which for DDR
   signalling is **half** the per-lane bit rate. The sibling drivers'
   "1242MHz" mode name is the per-lane *bit rate*, so the right
   link-frequency is 621000000, not the 1242000000 the DT carries. TI's
   j721e-csi2rx doubles the advertised value back into an hs_clk_rate
   for the cdns-dphy-rx, so the SoC's D-PHY was banded for 2484 Mbps and
   received nothing. The trap: **LP-11 stop-state detection is
   rate-independent, so `VIDIOC_STREAMON` succeeds** (the RX PHY sees the
   N4's lanes idle correctly) and the failure is a silent
   zero-frames-forever hang — `poll()` just never fires. Fixed in
   `mycam004m.c` by halving the DT value at parse time (comment in the
   code explains why the DT keeps carrying the physically-meaningful bit
   rate). The "sustained streaming for 25+ seconds" claim in the entry
   above did NOT survive re-testing on a clean boot: a minimal
   REQBUFS/QBUF/STREAMON/poll tester (`camgrab.c`, cross-compiled
   statically with the Yocto toolchain, kept on `/data/camgrab`)
   showed STREAMON-ok + zero frames on every attempt until this fix.

2. **N4's MIPI TX needs a PLL/PHY latch pulse and the BANK20 output
   arbiter initialized — both absent from N4's own datasheet and from
   this driver.** The sibling Allwinner driver's `mipi_tx_init()` does
   `0x44=0x00, 0x49=0xF3, 0x49=0xF0, 0x44=0x02, 0x08=0x40` (bank 0x21)
   right after the PLL block — N4's datasheet register table skips
   0x44/0x49 entirely (it's a preliminary Rev 0.0). And its `arb_init()`
   programs the bank-0x20 arbiter that funnels the 4 decoded channels
   into the TX: disable (0x00=0x00), latch config (0x40=0x01,
   0x0F=<dtype 0x00 for YUV422>, 0x0D=0x01, 0x40=0x00), enable
   (0x00=0xFF = vendor's 0x11<<ch for all 4). Verified live over i2cset:
   without these, zero frames; with them, frames flow. A bare re-enable
   write after disabling the arbiter is NOT enough — the full
   disable/latch/enable sequence is required (verified by switching it
   off and on). Both blocks are now appended to
   `mycam004m_csi_output_regs[]` in `mycam004m-regs.h`, module rebuilt
   and installed onto the SD rootfs (takes effect next boot; the current
   boot runs the equivalent via `/data/mycam-tx-arb.sh`).

3. **End-to-end proof**: with (1)+(2), `camgrab` captures full 1920x1080
   YUYV frames with incrementing sequence numbers, the frame content is
   the N4's no-video test pattern (green bars — pulled to the Mac,
   YUYV→PNG via ffmpeg, visually confirmed), and the app's CAMERAS
   screen shows **CAM 1 rendering that pattern live** (debug-hook
   screenshot) with CAM 2-4 correctly showing NO SIGNAL (their streams
   aren't routed — still the deliberately-deferred 4-route work).
   The N4 free-runs its timing generator on no-video, so frames flow
   even with zero locked inputs — the forced-pattern bit (bank5+ch
   0x69 bit5) is NOT required for this and isn't baked in anywhere.

Hard-won operational lessons from the same session:

- **`mycam004m`'s teardown path is broken on real hardware: `unbind`
  hangs forever and `rmmod` oopses the kernel** (refcount ends up -1,
  stack trace, tainted kernel — power-cycle required). The refcount
  itself behaves (drops to 0 after `rmmod j721e_csi2rx` releases the
  subdev), but the remove path then crashes. Until root-caused: to swap
  the module, replace the file under
  `/lib/modules/.../updates/mycam004m.ko` + `depmod -a` + reboot. Never
  `rmmod`/`unbind` it on a board you can't power-cycle.
- The N4's real BANK0 status map (datasheet p.32-33): `0xA0` =
  4-bit video-loss bitmask (vendor driver reads only this), `0xA4-A7`
  bit0 = NOVID per channel, `0xD0-D3` = AGC_LOCK, **`0xD4-D7` =
  CMP_LOCK**, `0xD8-DB` = H_LOCK, `0xDC-DF` = BW (b/w = no color
  burst). Useful discriminator this session: **CMP_LOCK=1 on all
  channels while H_LOCK=0 and NOVID=1** = the analog clamp locks onto a
  flat/DC line, i.e. the AFE is alive but there is *no video waveform
  on the pin* — config-side causes exhausted, physical-side confirmed
  by the user measuring **0V flat between socket pin1 (GND) and pin2
  (VIDEO) with a camera attached**.

**Where this leaves the camera bring-up**: digital path done and
verified; analog input is the sole remaining problem and it is
physical — the camera(s) put no signal on the VIDEO pin. Next checks
(user, multimeter): pin3→pin1 at the socket **with camera attached**
(is 5V actually flowing through the mated contacts under load), and
the camera plug's own wiring order (3 wires GND/VIDEO/DC in a
4-position plug — mirrored or offset insertion leaves the camera
unpowered while the socket still measures 5V). If those pass on
multiple sockets/cameras, suspect the cameras themselves.

### Addendum: analog investigation concluded — cameras are powered but mute (2026-08-23)

Continued from the entry above, same day. The camera-side current check
came back: **each AHD camera draws ~35mA when plugged in** (in-spec, the
Sony 225 sheet says 50mA working) — so the cameras power up, killing the
"plug wiring leaves the camera unpowered" theory. The camera's video
wire surfaces on socket pin 4 (NC on the board) at a stiff ~2.6-2.85V DC
that sustains ~10mA when bridged into the board's 75R termination — an
alive output *stage*. But three independent probes all say there is no
video modulation on it:

- ~0.08V AC on a DMM (ambiguous alone — DMM bandwidth is far below video
  line rate, so real video and dead bias can both read this),
- the N4, fully vendor-configured, with the bridge's contact *verified
  live by the meter reading ~10mA in series the whole time*, swept
  through every AHD_MD mode: NOVID/H_LOCK/AGC/BW never moved once, and
- **the killer: the wire's DC level does not respond to light at all**
  (lens covered vs phone flashlight straight in — a real video signal's
  DC average tracks scene brightness; this one is rock solid). Also
  reproduced identically on a second bench supply (5.1V under load), so
  supply marginality is out.

A TVI-labeled camera was also tried (voltage rating unknown — possibly
a 12V unit that never started); zero reaction from the chip.

**Verdict: these cameras' imaging pipelines are dead/mute — powered,
biased, producing no video. All units behave identically, so it reads
as a bad batch or harness-family problem, not one dud.** Caveat recorded
for honesty: with the output dead, the earlier "video lands on NC pin 4"
deduction is no longer certain — a dead output correctly wired to pin 2
would also read 0V flat there. The pinout question only becomes
answerable with a camera that actually modulates.

**Next step when hardware allows**: one known-good AHD 1080p camera with
transparent (non-sealed) wiring — video+GND to socket pins 2/1, power
separate. Everything downstream is verified waiting: the driver alone
(fresh boot, zero manual scripts) brings up the full
N4→MIPI→CSI2RX→V4L2→app chain and streams the N4's free-run frames to
the CAMERAS screen, and the chip-side lock registers plus the on-device
screenshot hook give an instant verdict the moment a real signal
arrives. If the cameras came bundled with the MY-CAM from MYIR, this is
a supplier-support claim.

Chip-register knowledge bank gained (bank5+ch reg 0x69, from live
experiments): bit7 = "emit nothing on no-video" (setting it stopped the
free-run frames entirely); bits 5/4 (pattern force / mem enable) are NOT
needed for free-run frames; and zeroing bank0 0x1C+ch does *not* expose
raw ADC content — the unlocked channel's output stays fully synthetic,
so the "use the capture path as a scope" idea is a dead end on N4.

## First real picture through the MY-CAM004M path (2026-08-24) — camera works; three driver bugs found, all reproduced/fixed live over I2C, none baked in yet

A known-good 1080p AHD source was attached to input 0 (it turned out to be a
"BY-J" 360°/surround-view box's composite output — car top-view + three
fisheye tiles + Chinese OSD — a real, stable 1080p30 AHD signal either
way). Session was entirely live over SSH with `i2cset -f`/`i2cget -f` and a
new grabber; **nothing here is in the driver/image yet** — see "What to
change" at the end.

**Gotcha first: the board had booted from eMMC** (no USR held), which still
has the pre-08-23 image (fake backend, no `camera@31` DT node, no
`/dev/i2c-4`). Everything below is on the SD build. Check
`mount | grep " / "` says `mmcblk1p2` before trusting any camera result.

### Result

- Driver probes as before (`DEV_ID 0xb0`, `link-freq 621000000`), pipeline
  oneshot runs, `/dev/mycam/cam1` → `/dev/video4`. **But with the driver's
  own baked-in init alone the N4 never locks**: bank0
  `NOVID=1 CMP_LOCK=1 H_LOCK=0` on all 4 channels — the exact "AFE alive,
  no waveform" signature the 08-23 session saw. Replaying the vendor AHD
  bring-up (`/data/mycam-vendor-replay.sh`, banks 0/1/5-8/9/10/11/13, ~180
  regs/ch) **immediately** gives `VLOSS 0x0f→0x0e`, ch0 `NOVID=0` — proving
  the shipped `mycam004m_init_regs[]` (bank-0 AHD_MD writes only, no AFE
  bring-up) was a real bug that would make *any* camera, dead or alive,
  read as mute.
  **Correction (2026-08-24, later): this does NOT reopen the 08-23
  "cameras are mute" verdict.** That session already replayed this exact
  same ~180-reg/ch AFE sequence live over i2cset (not baked into the
  driver, but tried) and still got no lock on the actual camera units —
  and, independent of any register config, measured the camera's video
  line holding a static DC bias that did not move at all between a
  covered lens and a flashlight straight into it, across every unit
  tested. That's driver-independent physical evidence, not an artifact of
  this config gap. The source used in *this* session (below) is a
  different, known-good device (a "BY-J" surround-view box), not the
  original camera modules — so this proves the driver/pipeline path is
  correct, not that the 08-23 cameras are alive. See NOTES.md's `## First
  real picture...` closing note and the 08-23 entries above for the full
  case; those units are still presumed dead until retested.
- With `AHD_MD=0x03` (1080p25, the driver's default) ch0 gets `NOVID=0`
  but `H_LOCK=0`; with **`AHD_MD=0x02` (1080p30)** ch0 reads
  `NOVID=0 H_LOCK=1 AGC_LOCK=1 BW=0` — full colour lock. This source is
  30p (NTSC-land); the driver must not hard-code 25p.
- **Byte order is UYVY, not YUYV.** Raw frames start `80 xx 80 xx` = U/V at
  0x80 with luma in the odd bytes; decoded as UYVY on the Mac (`ffmpeg -f
  rawvideo -pix_fmt uyvy422`) the frame is a perfect picture, decoded as
  YUYV it's the green/magenta mess the app shows. The vendor Allwinner
  driver declares `MEDIA_BUS_FMT_UYVY8_2X8` too. Driver advertises
  `YUYV8_1X16` (3 places in `mycam004m.c`), the pipeline script sets
  `YUYV8_1X16` on both SoC entities, and `camerafeed.cpp` requests/decodes
  `V4L2_PIX_FMT_YUYV` — all three need to change together.
- **The arbiter enable `0x00=0xFF` (all 4 channels) interleaves the 3 idle
  channels' frames into stream 0.** Symptom: V4L2 buffers complete at
  ~90-100/s, each with only 30-550 of 1080 lines DMA-written (the rest
  is untouched buffer memory = `Y=0,U=0,V=0` = the dark-green band in the
  app), consecutive buffers alternating between real video, black and
  colour bars (`5a 51 f0 51` = the vendor bring-up's `EX_CBAR_ON`
  pattern on no-video channels). The RX is not filtering by VC — or the
  N4 isn't putting the channels on distinct VCs; not yet determined which
  (see open question). Latching the arbiter for **ch0 only** (`0x00=0x00,
  0x40=0x01, 0x0f=0x00, 0x0d=0x01, 0x40=0x00, 0x00=0x11` — the full vendor
  `arb_init` sequence; a bare `0x00=0x11` write kills output entirely)
  gives a rock-solid **30 buffers/s, every one exactly 1080 lines**, for
  as long as it was left running. The saved frame from that run is the
  proof image (`scratchpad/frame6-uyvy.png` in the session; a copy was
  handed to the user).
- Frame-rate/line-count measurements above come from a new tool,
  **`/data/camgrab2`** (Rust, static aarch64-musl — see "Tooling" below):
  zero-fills each buffer before queueing and reports how many lines the
  DMA wrote, per-frame `dt`, and saves the *last* frame instead of the
  first (the original `camgrab` saves frame 0, which is always pre-lock
  because the driver re-runs its init at STREAMON — see next point).
  Usage: `camgrab2 /dev/mycam/cam1 <nframes> <out|-> [yuyv|uyvy]
  [save_index]`. First attempt scanned the whole 4 MB buffer for the last
  non-zero byte — on this SoC the vb2-dma-contig mmap is uncached and
  that took ~700 ms/frame, starving the DMA and producing bogus
  "every buffer partial at 1.3 fps" numbers; it now samples only the last
  8 bytes of each line. Don't byte-scan DMA buffers on this board.
- **The driver re-applies `init_regs` + `csi_output_regs` on every
  first-stream enable** (`mycam004m_enable_streams()`, `!stream_enable_mask`)
  — i.e. every STREAMON resets `AHD_MD` to 25p and the arbiter to 0xFF.
  Any live fix therefore has to be poked *after* STREAMON, and the arbiter
  re-latch briefly stops the TX, which makes `camerafeed`'s stall
  reconnect kick in → reopen → STREAMON → init again. So the app cannot be
  shown the good config live; the on-device screenshot with the good
  config applied under a running app was still striped/green. `camgrab2`
  tolerates the restart fine (full frames resume within ~1 s).

### Open question (matters once 4 cameras are attached)

Whether stream 0 received all four channels because (a) the N4 puts all
channels on VC0 with `MIPI_CH_ID_AUTO`, or (b) the TI cdns-csi2rx stream
isn't VC-filtering (mainline's `CSI2RX_STREAM_DATA_CFG_VC_SELECT` vs TI's
multistream routing via `get_frame_desc`). Partial evidence, late in the session: the Cadence driver copy in the
Yocto volume (`linux-libc-headers/6.6/.../cdns-csi2rx.c`, NOT the 6.12
linux-ti-staging tree — that one still needs checking) programs
`CSI2RX_STREAM_DATA_CFG_EN_VC_SELECT | VC_SELECT(i)` per stream, i.e.
stream i accepts only VC i. If 6.12 does the same, the RX *is* filtering
and the interleaving means the N4 emits all four channels on VC0 —
case (a), an N4 register problem (`MYCAM004M_REG_MIPI_CH_ID_TYPE` /
`MIPI_CH_ID_AUTO` may not mean what the driver comment says). If
(a), the fix is N4-side VC assignment; if (b), the per-input arbiter
enable is the *only* isolation and the 4-route pipeline work is blocked
on it. Either way the arbiter mask should track enabled inputs, not be
`0xFF`.

**Resolved (same day):** the 6.12 linux-ti-staging `cdns-csi2rx.c`
(work-shared kernel-source in the build volume) programs per-stream VC
filters from the source's frame descriptors
(`csi2rx_update_vc_select()`), same as the 6.6 copy — the RX *is*
filtering, so this is case (a): the N4 emits all arbiter-enabled
channels on VC0. The arbiter-mask fix below is what isolates
single-camera capture; simultaneous multi-camera needs N4-side VC
work.

### What to change (IMPLEMENTED and hardware-verified the same day —
see the matching section below; list kept for the reasoning)

1. `mycam004m-regs.h` `init_regs`: add the vendor AHD bring-up (the
   contents of `/data/mycam-vendor-replay.sh`, minus its 30p/25p arg) and
   set `AHD_MD` to `0x02` (1080p30) — or better, make fps a DT/module
   parameter. Delete the "25P kept because…cameras are mute" comment;
   its premise is refuted.
2. Arbiter: drop the trailing `{ ARB, 0x00, 0xff }` from
   `csi_output_regs`; do the full disable/latch/enable sequence in
   `mycam004m_enable_camera_input()`/`disable_camera_input()` with
   `en = 0x11 << input` OR'd over enabled inputs (vendor `en_param`).
3. `MEDIA_BUS_FMT_YUYV8_1X16` → `MEDIA_BUS_FMT_UYVY8_1X16` in
   `mycam004m.c` (3 sites), `mycam004m-configure-pipeline.sh` (both
   `-V` calls), and `camerafeed.cpp` requesting/decoding
   `V4L2_PIX_FMT_UYVY` (its YUYV→RGB loop needs the byte offsets
   swapped; the fake backend's `.bin` frames were captured as YUYV and
   would need re-capturing or a swap).
4. Re-verify with `camgrab2` (30/s, 1080 lines, `NOVID=0 H_LOCK=1`) and the
   app screenshot hook — the two are different claims (see 08-23).

### Tooling notes from this session

- Docker/OrbStack wedged mid-session (every `docker`/`orbctl` call hung,
  even `orbctl restart docker`); `osascript quit` + `pkill -9 OrbStack` +
  `open -a OrbStack` brought it back in ~5 s. Kill any backgrounded
  shells that are stuck on a docker call first.
- The board's `python3` is trimmed (no `fcntl`, `mmap`, `ctypes`) — useless
  for V4L2. Board tools can be cross-built **without Docker**: `rustup
  target add aarch64-unknown-linux-musl`, then `RUSTFLAGS="-C
  linker=<toolchain>/lib/rustlib/aarch64-apple-darwin/bin/rust-lld -C
  linker-flavor=ld.lld -C link-self-contained=yes -C
  target-feature=+crt-static" cargo build --release --target
  aarch64-unknown-linux-musl` (plain `cargo build` picks macOS `cc` as
  the linker and fails). Source for `camgrab2` lived in the session
  scratchpad; worth moving into `~/code/mycam004m/tools/` next time.

## Camera driver fixes implemented + verified end-to-end (2026-08-24, later the same day)

All three fixes from the previous section's "What to change" list are
implemented, built, deployed, and hardware-verified. The app's CAMERAS
screen now renders the live camera correctly.

### What changed

- **Driver (`~/code/mycam004m`, separate repo):**
  - The bank-0-only `mycam004m_init_regs[]` table is gone. The full
    vendor AHD/AFE bring-up (the contents of `/data/mycam-vendor-replay.sh`,
    ~140 writes/channel plus chip-wide init) now lives as code in
    `mycam004m.c` (`mycam004m_init_video_inputs()` /
    `mycam004m_init_video_channel()`) -- code, not a table, because it's
    parameterized by channel and needs read-modify-write. It runs **once
    at probe**, not at STREAMON: re-running it drops signal lock (~1s to
    re-acquire) and STREAMON is `camerafeed`'s routine reconnect path.
    Verified: after a cold boot, ch0 reads `NOVID=0 H_LOCK=1 AGC=1`
    before anything ever streams.
  - Frame rate is a module parameter (`fps=30` default / `fps=25`),
    resolved to `AHD_MD` 0x02/0x03 at probe. `MYCAM004M_FPS` back to 30.
  - The arbiter enable-all (`0x00=0xFF`) at the tail of
    `csi_output_regs[]` is gone; `mycam004m_arb_sync()` runs the full
    vendor disable/latch/enable sequence with a mask of exactly the
    streaming inputs at every stream start/stop. Verified: `ARB_EN`
    reads 0x11 after cam1 streams.
  - `MEDIA_BUS_FMT_YUYV8_1X16` -> `UYVY8_1X16` everywhere (3 sites).
  - Stream disable no longer powers down the AFE channel (`PD_VCH`) --
    isolation comes from the arbiter mask + TX off; powering down would
    black out every app reconnect while lock re-acquires.
  - `mycam004m-fake` follows the UYVY move: fourcc, comments, and the 4
    committed `cam*.bin` reference frames byte-swapped in place;
    `gen_fake_frames.py` emits UYVY now. Driver docs/README status
    sections rewritten (they claimed "never tested on hardware").
- **This repo:** `mycam004m-configure-pipeline.sh` sets `UYVY8_1X16` on
  both SoC entities; `camerafeed.cpp` requests/validates
  `V4L2_PIX_FMT_UYVY` and `convertUYVYToRGB32()` reads U/Y/V/Y byte
  order (renamed from convertYUYVToRGB32).

### Verification numbers (SD boot, cold)

- Probe: `found N4 decoder: DEV_ID 0xb0`, AFE init adds ~200ms,
  `link-freq 621000000 Hz`.
- `camgrab2 /dev/mycam/cam1 90 ... uyvy`: **90/90 frames, dt=33.3ms
  (exact 30fps), every frame bytesused=4147200 with all 1080 lines
  DMA-written**, no sequence gaps. Frame decodes to a clean picture
  with correct colors as UYVY.
- App end-to-end: `/tmp/ultima-camtest.request open` + screenshot hook
  -> CAM 1 tile renders the live picture (correct colors), CAM 2-4
  "NO SIGNAL" as expected with nothing attached.

### Two operational gotchas hit while deploying (worth remembering)

- **Don't try to live-reload the camera module stack.** `mycam004m`'s
  refcount is pinned while bound (v4l2 cross-module ref held by the
  `j721e_csi2rx` v4l2_device), and unloading consumer-first didn't work
  either: `rmmod j721e_csi2rx` hung in-kernel and left `mycam004m` at
  refcount **-1** (same family as the earlier "STREAMON hang + rmmod
  oops" note above). Files on disk were already updated, so the
  recovery was just a USR power-cycle. Deploy files -> power-cycle;
  skip module gymnastics.
- **The SD image regenerates its SSH host key every boot** (host keys
  live on volatile storage), so `ssh` fails strict host-key checking
  after every reboot. `ssh-keygen -R beagleplay-ti.local` +
  `-o StrictHostKeyChecking=accept-new` and move on.

### Still open (unchanged)

4 simultaneous cameras: the N4 emits everything on VC0 (case (a) above,
now confirmed since the 6.12 RX does VC-filter) -- expect N4-side VC
work (manual CH_ID mode?) plus the 4-route pipeline config before
multi-camera capture separates. Single-camera is solid.

## Flashed the built image; found + worked around a cold-boot I2C probe race (2026-08-24, later)

`flash.sh disk4` wrote the freshly-built `tisdk-base-image` (with all the
camera driver fixes from the sections above) to the SD card, boot
confirmed on `mmcblk1p2`.

**First true cold boot of this image hit a new failure**: `mycam004m`
probe failed with `-EREMOTEIO` reading the chip ID (`error -121`), so the
system fell back to nothing bound on `4-0031` (camera stayed on the fake
backend; `select-camera-backend.service`/`mycam004m-configure-pipeline.service`
both reported `failed`, having run before the real capture nodes existed).
`i2cdetect -y -r 4` moments later showed the N4 responding at 0x31
normally, and a manual re-probe (`echo 4-0031 >
/sys/bus/i2c/drivers/mycam004m/bind` -- safe here specifically because
probe had failed, so nothing was bound; this is NOT the documented
unbind-hangs/rmmod-oopses hazard, which is about tearing down an
already-*working* binding) succeeded immediately: chip ID read, full AFE
bring-up ran, ch0 locked (`NOVID=0 H_LOCK=1 AGC=1`). Restarting the two
services and re-running `camgrab2` + the app screenshot hook then
reproduced the exact same clean 30fps/1080-line capture and CAMERAS
screenshot as the live-patched test earlier the same day.

**Diagnosis**: every previous verification (this session and 08-23) was a
*warm* module reload/insmod on a board whose power rails, GPIOs, and I2C
bus had already been up and settled for a while. This was the first time
`mycam004m` probed during an actual cold power-on, where `omap_i2c` bus 4
comes up at t=0.275s but the probe (chip-ID readback) ran at t=2.582s and
still got a NAK -- i.e. the driver's own power-on delays
(`mycam004m_power_on()`: 5-10ms PWREN/LDO settle, 10-20ms RESET_N release,
both commented as "conservative margins" against datasheet minimums) were
apparently not enough margin on a genuine 0V-start cold boot, unlike a
warm reload where the rail was already at steady-state. One data point,
not yet reproduced a second time -- could also be probe-ordering/deferred-
probe timing unrelated to the LDO/RESET_N delays themselves.

**Not fixed yet** (driver still probes once, no retry) -- if this
reproduces on a future cold boot: either widen `mycam004m_power_on()`'s
delays, or add a bounded retry around the chip-ID readback in
`mycam004m_check_chip_id()`/`mycam004m_probe()` before failing hard. Filed
here rather than fixed blind since it's one occurrence.

## New "SZ-2053/3077S" surround-view cameras — pinout mismatch, not the driver (2026-08-25)

User switched to 4 new AHD cameras from an Amazon 360-kit ("SZ-2053/3077S 2K"
on the cable; Amazon B0FBFLSTB7, "BY-J 360-Degree Panoramic Camera System").
None locked: all 4 inputs `NOVID=1 CMP_LOCK=1 H_LOCK=0` across every AHD_MD
value — the "AFE alive, no waveform" signature again. Driver, services, and
single-route pipeline were all confirmed healthy (a known-good source still
produced a clean capture), so the fault was upstream of the board.

**Camera research.** These are the commodity 360-kit head: GalaxyCore **GC2053**
2MP sensor ("2053"), "3077" is the *lens* board (2053+3077 / 2053+6048 /
2053+1005 are interchangeable lens options), AHD encoder is a Fullhan-8536-class
part. Format is **AHD 1080p25** (PAL). MYIR's own recommended head (QJD 6048-2053)
is the same family at 5V. Every marketplace listing's spec table says "12V" — this
was **wrong for the actual hardware** and nearly sent us down a 12V-supply path;
see the memory note `verify-hardware-specs-not-seller-listings`.

**Root cause: connector pin-order mismatch.** User disassembled a camera — PCB
silkscreen `N/P · VO · G · 5V`, a **5V** camera (exactly what the MY-CAM jack
supplies). But the camera's moulded 4-pin barrel and the MY-CAM CJ-BM4-M11 jack
disagree on pin order. Camera plug, looking into the pins, key notch up:
`NC(top-left) 5V(top-right) / VID(bottom-left) GND(bottom-right)`. MY-CAM socket,
looking into the holes, key notch up: `GND(top-left) 5V(top-right) /
—(bottom-left, this is NC/pos-4) VID(bottom-right)`. Mated, every contact lands
on the wrong function; notably the camera's GND pin has no corresponding wire to
reach the socket's ground position. Signature while mis-mated: **0.7V across the
camera's G–5V pads** = 5V pushed through the camera's reverse-polarity clamp
diode (one forward drop) because GND and 5V are swapped — a clean diagnostic, and
a reason to unplug rather than leave it powered. Powered-but-unloaded VO floats
to ~1.1V DC (vs ~0.3–0.5V into the board's 75Ω termination) = video driving but
the yellow VID wire not landing on the socket VID pin.

**Fix.** Re-pin the camera barrel (or make a short cross-wired adapter) so
red→5V, black→GND, yellow→VID per the socket layout above. User moved the wires;
input 0 (jack **J1** → `/dev/video4` → `cam1`) then **fully locked**:
`NOVID=0 H_LOCK=1 CMP_LOCK=1 AGC_LOCK=1`, VLOSS `0x0f→0x0e`. `camgrab2` pulled
40/40 frames at a measured **40.0ms/frame (25.0fps)**, 1080 lines, real picture
content (fisheye view of the bench, not the 0x80/0x80 red fill). End-to-end
camera→N4→CSI2→ticsi2rx→V4L2 confirmed with a saved PNG.

**These cameras are 25fps — the driver default (fps=30, AHD_MD=0x02) does NOT
lock them.** At 0x02 they half-lock (NOVID=0 but H_LOCK=0, no usable video); only
AHD_MD=0x03 (1080p25) gives full lock. The live capture above required manually
poking `AHD_MD=0x03`; on a normal boot the driver programs 0x02 and the picture
is absent. **Persistent fix needed: load `mycam004m` with `fps=25`** — add
`options mycam004m fps=25` via a `modprobe.d` file in the Yocto layer (module is
listed in `modules-load.d/mycam004m-real.conf` today with no params). Not yet
committed; requires an image rebuild + reflash.

**Other observations.** The unpopulated `N/P` pad on the camera PCB is almost
certainly the AHD encoder's NTSC/PAL (30/25) select. Also worth doing on the
driver: at STREAMON, check a sentinel init register (e.g. bank1 0xCC==0x64) and
re-run the AFE bring-up if it was lost — an earlier attempt this session showed
the N4 reverting to power-on state (likely the 3 mis-wired cameras loading the
5V rail through their clamp diodes), which a reboot cleared; a self-heal check
would avoid the reboot.

## CameraGridScreen "3fps" — not a rendering perf bug, real root cause is upstream (2026-08-25)

User reported the 4-camera grid running at ~3fps with the new real cameras and
asked whether decoding at a smaller resolution would help. It already does
(`kDecimation=2`, see the 2026-08-17 fix above) — the actual cause turned out
to be almost entirely upstream of anything app-side resolution touches.

**App-side fixes made and hot-deployed (verified via the loop in "Fast
iteration loop" above), both real but neither the dominant lever:**
- `CameraFeed::convertUYVYToRGB32()` (now `...ToRGBA8888()`) writes
  `QImage::Format_RGBA8888` directly instead of `Format_RGB32` — removes a
  second full-image scalar `convertToFormat()` pass `SurroundTexture::upload()`
  used to do on the render thread, on top of the GUI-thread UYVY decode.
- `CameraFeed`'s reconnect timer now backs off exponentially (1s → 2s → 4s →
  capped 8s, reset to 1s on the next successful stream) instead of retrying a
  permanently-failing feed every flat 1s forever.

**Diagnosis, in order, with hardware numbers (all via
`/tmp/ultima-camtest.request open` + `journalctl -u ultima-app -f`, board at
`ultimagc.local`):**

1. **Only 1 of 4 feeds actually streams.** `cam2`/`cam3`/`cam4`
   (`/dev/video5`/`6`/`7`) fail `VIDIOC_STREAMON` with `ENODEV`, retried
   forever — this is the "Still open" multi-camera limitation two sections
   up (N4 puts every enabled channel on CSI2 VC0; only one route is isolable
   right now), not a new bug. `dmesg` shows no `mycam004m: enabling AHD
   input 1/2/3` lines at all — those three never even reach the driver's
   arbiter-enable call, they fail earlier in the media pipeline.
2. **Ruled out: GUI-thread contention from the 3 failing feeds' retry
   churn.** `strace -f -c -p <pid>` during the retry storm (pre-backoff)
   showed ~1.2s of `ioctl`/`mmap`/`close` time per 8s window, all on the
   single GUI thread `CameraFeed` runs on (open+ioctl+mmap+close per failed
   feed, ~3x/s). Real, and the backoff fix above is worth keeping
   regardless, but A/B testing it directly (same measurement before/after
   deploying the backoff fix) showed cam1's fps **unchanged** (~4fps both
   times) — this wasn't the bottleneck.
3. **cam1 itself: driver delivers ~12fps, app displays ~4fps of that** — both
   well under the 25fps `camgrab2` measured in isolation earlier this
   session (see above). Added a raw-arrival counter alongside the existing
   decoded-frame counter in `CameraFeed::onReadable()` (`fps arrived` vs
   `fps decoded` in the `[camerafeed]` log) specifically to split "driver/
   hardware not delivering frames" from "frames arrive but the app can't
   keep up converting them" — a single fps number conflates the two.
   Measured steady state: `~12.3 fps arrived, ~4.1 fps decoded` (added a
   `ULTIMA_CAM_FPS_LOG=1` opt-in env var to `CameraFeed` for this — silent
   by default, doesn't ship as a permanent per-2s heartbeat log). With only
   `kNumBuffers=4` capture buffers, `onReadable()`'s drain-all-take-newest
   loop is discarding ~2 of every 3 arriving buffers per wake, meaning the
   GUI thread is only getting scheduled to service this camera's socket
   notifier ~4x/sec. Caveat: "12.3 fps arrived" is what the app observed
   dequeuing buffers, not a confirmed driver ceiling — with only 4 buffers
   and a slow-draining consumer, the driver may itself be stalling/dropping
   frames waiting for a free buffer, so this could still be downstream of
   the same GUI-thread-scheduling issue rather than an independent hardware
   number. Not distinguished from the true DMA-completion rate here.
4. **Not CPU-bound.** `top` during a streaming window: system 72% idle,
   `ultima-app` at 26% CPU on a single core. Per-thread jiffies from
   `/proc/<pid>/task/*/stat` show both the main/GUI thread and
   `QSGRenderThread` mostly idle, not pegged. Rules out "conversion/QML
   work is too slow" as the explanation for the 12→4fps drop — something is
   making the GUI thread wait (render-thread sync barrier? GPU/PowerVR
   driver stall on the 4 separate `QQuickFramebufferObject` FBOs each doing
   their own `glTexSubImage2D`+draw?), not spend CPU. Not yet root-caused —
   would need GPU-side profiling this image doesn't have tooling for
   (`perf`, `v4l2-ctl`, `camgrab2` are all absent from the deployed rootfs).

**Bottom line:** resizing further wouldn't have moved either of the two real
bottlenecks (the driver-level single-camera-at-a-time limit, or whatever's
throttling cam1's own display rate to ~4fps independent of CPU). The
1920x1080→960x540 decode already in place was the correct lever for the
2026-08-17 problem (GPU texture-reallocation-per-frame + full-res conversion
cost) — this is a different problem sitting upstream of it.

**Superseded 2026-08-26:** the ~12→4fps mystery was root-caused (uncached DMA
buffer reads — nothing GPU-side at all) and fixed the next day; see "Camera
framerate: root cause found, 4fps → 25fps" below. The "12.3 fps arrived"
number was indeed downstream of the same cause, exactly as the caveat above
suspected — with the consumer fixed, arrival is a clean 25.0fps. The N4-side
VC work for 2-4 simultaneous cameras remains open.


## Camera framerate: root cause found, 4fps → 25fps (2026-08-26)

Continuation of the entry above. Systematic experiment pass on real hardware
(one AHD camera attached, `fps=25` driver config), hot-deploying app builds
via a `/run/systemd/system/ultima-app.service.d/` `ExecStart=` override
pointing at `/tmp/exp/` binaries — zero rootfs remounts during the whole
experiment loop, worth reusing (the drop-in dir is tmpfs; the final install
is the only remount).

### Root cause of the ~4fps: uncached V4L2 buffer reads

New per-stage timers (gated behind the existing `ULTIMA_CAM_FPS_LOG`) split
one frame's cost into DQBUF-drain / UYVY→RGBA convert / GPU upload /
render. The convert alone was **~240ms per frame** — everything else was
0.05-5ms. The `videobuf2-dma-contig` MMAP buffers are mapped **uncached**
(dma-coherent, Normal-NC), and the scalar converter does ~1M single-byte
loads per frame from that mapping; every load is a full DRAM round-trip
(~240ns). The GUI thread spent ~240ms inside each conversion, so it only
serviced the socket notifier ~4x/s — which also explains the "12fps
arrived" from yesterday (3 buffers piled up per 240ms wake = the driver
dropping the rest on the floor with no free buffer; the true delivery rate
was always 25fps). "Not CPU-bound, 72% idle" was a misread of busybox `top`
averages: one core was pegged in stalled loads, the other three idle.

Fix is two independent halves, both applied:

1. **Ask vb2 for CPU-cacheable buffers** — `VIDIOC_REQBUFS` with
   `V4L2_MEMORY_FLAG_NON_COHERENT` (`req.flags`, kernel ≥5.15). Works with
   NO driver change: `j721e-csi2rx` already sets `q->allow_cache_hints=1`
   (few drivers do — check `caps` echo for
   `V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS`, 0x40, and that `flags` echoes
   back 0x1). vb2 cache-invalidates on DQBUF itself; the CPU never writes
   the buffers so there is no dirty-line hazard.
2. **NEON conversion** (`vld4q_u8` — one 64-byte de-interleaving load per
   16 UYVY pairs) instead of byte-at-a-time scalar.

Measured convert cost per 960x540 frame, same camera, same board:

| variant | convert ms/frame | decoded fps |
|---|---|---|
| scalar, uncached buffers (shipping code until today) | 240 | 4.1 |
| memcpy rows to cached scratch + scalar | 35 | 25 |
| NEON directly from uncached buffers | 64 | 15 |
| scalar, **cached (non-coherent) buffers** | 8.9 | 25 |
| NEON, **cached buffers** | **3.6** | **25** |

(That middle row is why the "never byte-scan DMA buffers" memory note
existed — wide loads help even uncached, but cached is the real fix.)

### Second bottleneck (4-camera scaling): the render thread — fixed with zero-copy dma-buf import

25fps single-camera achieved, but a 4-camera load proxy (`ULTIMA_CAM_FANOUT=1`
env: points cameraFeed2..4 at cam1's object so all four grid quadrants
render real frames — kept in main.cpp as a bench tool) showed the render
thread saturating at **~9.5fps per quadrant, 104% of a core**: 4x
`glTexSubImage2D` of 2MB RGBA each + 4 FBO passes. Upload bytes scale it
(decimation 3 → 15fps, 4 → 19fps) but nothing upload-shaped reaches 25.
Double/triple-buffering the destination textures did nothing (not an
in-use stall).

The fix that ends the category: **import the V4L2 buffers into the GPU
directly** (`VIDIOC_EXPBUF` → dma-buf → `eglCreateImageKHR`
(`EGL_LINUX_DMA_BUF_EXT`, `DRM_FORMAT_UYVY`, BT.601 narrow-range hints) →
`GL_TEXTURE_EXTERNAL_OES`), sampler does UYVY→RGB in-GPU. This PowerVR
stack (Mesa-based DDK 25.2) supports it: `EGL_EXT_image_dma_buf_import` +
`GL_OES_EGL_image_external_essl3` + UYVY pipe format, all confirmed live
on-target. Measured, 4 quadrants at 1080p25:

| path | per-quadrant fps | render thread |
|---|---|---|
| convert + glTexSubImage2D | 9.5 | 104% (saturated) |
| **zero-copy external texture** | **25.0** | **22%** |

Single camera: render thread 51% → 13%, `render()` 0.4ms, and the CPU
convert disappears entirely while nothing needs QImages. Whole app during
1-cam streaming: ~15% of one core total.

### What shipped (see the code comments for the load-bearing details)

- `camerafeed.{h,cpp}`: capture+convert moved off the GUI thread to a
  per-feed capture thread (GUI was also `blockedForSync` 10-30ms/frame by
  the render thread — QSG_RENDER_TIMING showed the threaded-loop handoff);
  non-coherent (cached) buffers; NEON convert; zero-copy buffer
  lending mailbox (publish/acquire/release with refcounts, retired buffers
  QBUF'd back on the capture thread). Converted QImages still exist but
  only while a consumer registers (`addFrameConsumer`).
- `dmabuftexture.{h,cpp}` (new): EGLImage/external-texture import,
  libEGL via dlopen (no link dep, macOS build untouched), session
  tracking so a stream restart drops stale imports (EGLImages pin CMA
  until released — 6 buffers x 4MB x 4 cams = 96MB of the 128MB CMA pool;
  if a hidden view misses the drop window, tryOpen's reconnect retries
  1s later and self-heals. Raising CONFIG_CMA_SIZE_MBYTES is the
  follow-up if 4 real cameras ever hit REQBUFS ENOMEM here).
- `cameraview.cpp`: renders the external texture when available (grid
  tiles AND the mirror overlays — `mirror.frag` works unchanged via
  `ShaderManager::ExternalSampler`, which rewrites `sampler2D` →
  `samplerExternalOES` at load); QImage path kept as fallback + for the
  macOS sim.
- `surroundview.cpp`: unchanged rendering (warp mesh samples converted
  QImage textures — a zero-copy variant is possible later but the 360
  screen is an overlay, not the steady state); registers/deregisters as a
  frame consumer with visibility.
- `ULTIMA_CAM_ZEROCOPY=0` forces everything through the converted path
  (A/B lever + escape hatch); `ULTIMA_CAM_FPS_LOG=1` prints per-feed
  arrived/published/decoded rates + convert times, and per-view render
  stats.

Colors note: the GPU sampler decodes BT.601 **limited-range** (hinted at
import, matching what the N4 actually emits per the media graph's
`quantization:lim-range`); the CPU path always treated the data as
full-range, so zero-copy frames show slightly more contrast — that's the
*more correct* rendering, not a regression.

### Hidden QQuickFramebufferObjects still render — update() bypasses visibility

First full-app 4-cam fanout runs came in at 17fps, not the prototype's 25.
Per-renderer identity logging showed **seven** CameraViews rendering, not
four: the two mirror overlays (hidden, 711x400) and the rear camera screen
(hidden, 1280x720) were re-rendering every frame alongside the grid.
`QQuickFramebufferObject::update()` schedules the FBO render pass regardless
of item visibility — a `frameReady -> update()` connection on a hidden item
is not free, it's a full FBO pass. The two hidden *mirror* views were the
expensive part: mirror.frag's per-fragment fisheye reprojection over
scattered 1080p UYVY external-texture fetches is GPU-heavy (QSG swap time
44-61ms with them "hidden" — GPU-bound, not CPU). Two fixes, both needed:

- `cameraview.cpp`/`surroundview.cpp`: the `frameReady`/`streamingChanged`
  handlers now check `isVisible()` before calling `update()`.
- `CameraGridScreen.qml`: the screen "closes" by sliding to
  `x: -parent.width`, which leaves `visible: true` — so the `isVisible()`
  guard alone still let all 4 tiles render off-screen whenever any feed
  streamed (turn signal on with the grid closed = 4 wasted FBO passes per
  frame, measured as 4 extra 25/s renderers). It now binds
  `visible: isOpen` — the same pattern RearCameraScreen/Camera360Screen
  already used with opacity.

With both: fanout grid open = exactly 4 renderers at 25.0/s; grid closed
with a mirror overlay up = exactly 1. A *visible* mirror overlay still
costs real GPU (grid + overlay both up measured ~16fps whole-window;
overlay over the plain gauge screen runs 28fps) — if that ever matters,
the mirror views could sample the converted 960x540 QImage path instead of
raw 1080p UYVY, trading a CPU convert (already paid whenever the 360 view
is also up) for the scattered GPU fetches. Not done now: the overlay is
indicator-gated and transient.

Verification on hardware (exp binaries; final installed build verified
same-day below): grid at 25.0fps displayed with 1 camera and with the
4-quadrant fanout proxy; 360 stitch + screenshots correct on the converted
path; no `requeue` errors in the journal across open/close cycles.

Final install (same day): the verified exp binary went to
`/usr/bin/ultima-app` via the one remount-rw/mv/remount-ro window (no
"mount busy" hit — the running binary was in `/tmp/exp/`, so `/usr/bin`
wasn't text-busy), drop-in removed, `daemon-reload` + restart. Confirmed
on the installed build: md5 matches the verified binary, `/` back to `ro`,
service active on the stock ExecStart, grid streaming at ~25fps
(screenshot), fanout checks all green — exactly 1 renderer with only a
mirror overlay up, exactly 4 with the grid open, 0 after close, 360 view
decoding at 25fps.

## BeagleY-AI (AM67A/J722S) bring-up — milestone 1: Yocto build/app port

**Status (2026-08-29): board in hand, image flashed to SD — not yet booted/
verified.** `tisdk-base-image` builds clean with `ultima-app` packaged, `.wic`
produced with the expected 3-partition layout, and is now written to the SD
card that'll go in the board. See `docs/BEAGLEY-AI-EVAL.md` for
the paper evaluation (why switch boards at all — the AM67A Wave5 hardware
encoder unblocking 4-camera dashcam recording) and the local plan at
`/Users/jellis/.claude/plans/nested-floating-wand.md` for the full milestone
breakdown. This section is the build-log detail for getting there.

**Layer layout note (2026-08-28, same day):** all BeagleY-AI-specific Yocto
layer content described below lives in a new top-level `beagley-ai/` directory
(`beagley-ai/meta-ultima-beagley-ai-src/`), a **sibling** of this
`beagleplay-falcon/` directory — not inside it, since this directory is named
specifically for BeaglePlay. Same "unrelated concerns" reasoning already used
to keep `meta-falcon-beagleplay-src` apart from `meta-ultima-beagleplay-src`
(see "Fix: custom layer `meta-falcon-beagleplay`" above). What stays shared
in `beagleplay-falcon/`: the Docker/TI-SDK build tooling (`Dockerfile`,
`run.sh`, `build.sh`, the `falcon-yocto-build` docker volume) — genuinely
board-agnostic infrastructure both boards already build through, not something
either board owns. File paths below were written during initial bring-up
(single-layer) and have been corrected to their final split-layer locations;
where the split itself surfaced something new, that's called out explicitly.

### The machine already exists — no new Yocto layer needed

`meta-ti/meta-beagle` (already vendored into the `falcon-yocto-build` docker
volume, already in `bblayers.conf`) ships `beagley-ai.conf` /
`beagley-ai-k3r5.conf` (`require j722s.inc` + `beagle-bsp.inc`). Unlike the
original Falcon work, this did **not** need a from-scratch machine layer.

Two things that make it a genuinely different build target from `beagleplay-ti`,
not just a different `MACHINE=` value:

- **It's locked to the BeagleBoard.org kernel/u-boot, not TI-staging.**
  `beagle-bsp.inc` only defines `bb_org-6_12` (default) / `bb_org-6_6` BSP
  variants: kernel `linux-bb.org` 6.12, u-boot `u-boot-bb.org` 2025.x. There is
  no `ti-*` BSP variant for beagle machines. Consequence: our un-scoped
  `linux-ti-staging_%.bbappend` (carrying the AM625 CAN/camera DT patches)
  simply never fires for this machine — no patch-apply failure, it's just
  absent, since beagley-ai builds a different kernel recipe entirely.
- **The R5/TIFS boot chain is HS-FS (High-Security Field-Securable), not GP.**
  Confirmed by the produced artifact name:
  `tiboot3-j722s-hs-fs-evm-beagley-ai-k3r5-2025.10+git-r0.bin` (also set
  explicitly: `beagley-ai-k3r5.conf` has `SYSFW_SUFFIX = "hs-fs"`). BeaglePlay's
  AM625 boots GP. This built and produced a working artifact set with zero
  extra key-provisioning on our part — meta-beagle must ship default/dev
  signing for this — but it's new territory this project hasn't exercised, and
  it directly affects the eventual Falcon-fork work (Falcon needs to rebuild the
  R5 SPL, which now goes through this signed chain rather than AM625's simple
  GP build). Falcon is **not** wired for j722s at all yet (meta-ti's
  `u-boot-ti.inc` only prepends the falcon package for the four `am62*-evm`
  machines), and even if it were, it would need porting to `u-boot-bb.org`
  rather than reusing `meta-falcon-beagleplay` (which patches `u-boot-ti-staging`)
  — a real, separate fork of that work, not attempted here.

### Two build-blocking gaps found and fixed (both real, not speculative)

1. **`tisdk-uenv` recipe gap.** `meta-arago-distro`'s `tisdk-uenv.bb` (pulled
   into every arago image unconditionally via `arago.conf`, not machine-scoped)
   ships a `uEnv-sk.txt` template only for TI's own EVM machine name
   (`j722s-evm`), never extended to BeagleBoard.org's community machine name
   (`beagley-ai`). Same class of gap as the original Falcon wiring (a TI SDK
   recipe wired to EVM names only) — see "What was actually wrong upstream"
   above. Fixed with `beagley-ai/meta-ultima-beagley-ai-src/recipes-tisdk/tisdk-uenv/`:
   a `tisdk-uenv.bbappend` + `tisdk-uenv/beagley-ai/uEnv-sk.txt`.
   - **Real gotcha hit along the way**: an empty/comment-only `.bbappend` does
     **not** get your layer's directory added to `FILESPATH` — `FILESEXTRAPATHS`
     is not auto-extended just because a `.bbappend` exists for a recipe.
     `${THISDIR}` in *another* layer's existing bbappend (meta-ti-foundational's,
     in this case) is immediate-expanded (`:=`) to *that* layer's own directory
     at parse time and never picks up other layers. Our bbappend has to add its
     own `FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"` explicitly.
   - **This same gotcha resurfaced, and mattered more, once the layer was
     split** (see "Layer layout note" above): `ultima-app.service`'s
     beagley-ai override originally lived in the *same* layer as
     `ultima-app.bb` (`recipes-ultima/ultima-app/files/beagley-ai/`), where it
     needed **no** extra `FILESEXTRAPATHS` line — `FILESPATH`'s default search
     already includes `${FILE_DIRNAME}/files` from a recipe's *own* directory,
     auto-suffixed per active override, no bbappend content required beyond
     the recipe itself. Once split into the separate
     `beagley-ai/meta-ultima-beagley-ai-src` layer, that stopped being true —
     it's now a genuinely cross-layer override (a different layer's
     `.bbappend` contributing a file for a recipe defined elsewhere), so it
     needs the exact same explicit `FILESEXTRAPATHS:prepend :=
     "${THISDIR}/${PN}:"` treatment as the tisdk-uenv fix. See
     `recipes-ultima/ultima-app/ultima-app.bbappend` in the new layer.
2. **A leaked `local.conf` line.** Seeded the `build-beagley-ai` build directory
   by copying `beagleplay-ti`'s working `local.conf` verbatim (fast path — same
   `DISTRO`/`IMAGE_INSTALL` baseline, different `MACHINE`), which brought along
   `DISTROOVERRIDES:append = ":ti-falcon"`. That line unconditionally requires
   `u-boot-ti-staging-falcon` (`ti-falcon.inc`'s `IMAGE_INSTALL:append`, itself
   unscoped) regardless of machine — broke the build with "Nothing RPROVIDES
   u-boot-ti-staging-falcon" since beagley-ai has neither Falcon wiring nor a
   `u-boot-ti-staging` recipe at all. Commented out for the beagley-ai build
   dir only (`beagleplay-ti`'s own `local.conf` untouched).

Also hit and fixed: forgot to bind-mount the real `../ultima-app` host source at
`/home/builder/yocto/ultima-app-src` in the ad-hoc `docker run` invocations used
for this bring-up (unlike `build.sh`, which always does). A prior session's
`build.sh` run had left an empty directory *placeholder* at that exact path
inside the `falcon-yocto-build` volume (Docker auto-creates bind-mount target
directories, and they persist in the underlying volume after the container
exits even though the bind-mounted content doesn't) — so `ultima-app.bb`'s
`do_unpack` silently "succeeded" copying nothing, and the failure only surfaced
one task later at `do_configure` ("no `.pro` file found"). Fixed by adding the
same `-v "$ULTIMA_APP_SRC:/home/builder/yocto/ultima-app-src:ro"` mount
`build.sh` uses.

### Build setup used — now folded into `build.sh` (2026-08-28, later same day)

A second, independent BitBake build directory, `tisdk/build-beagley-ai/`
(sibling to the existing `tisdk/build/`), seeded by copying
`build/conf/{local.conf,bblayers.conf}` then editing `MACHINE` and commenting
out the `ti-falcon` `DISTROOVERRIDES` line. `bblayers.conf`'s `BBLAYERS` also
needs `.../tisdk/sources/meta-ultima-beagley-ai` added (alongside the existing
`meta-ultima-beagleplay` entry — both layers are always synced in, harmless for
either machine since each only contributes override-scoped content the other
machine's build never matches). `SSTATE_DIR` pointed at the existing
`build/sstate-cache` (not the new dir's own) to reuse architecture-common sstate
(aarch64 toolchain, common non-machine-specific recipes) across both machines —
worked as intended; a large fraction of the ~8918-task build already existed in
shared sstate. `DL_DIR` (`${TOPDIR}/../downloads`) was already shared
automatically since both build dirs are siblings under `tisdk/`.

**Now reproducible from a fresh volume via `build.sh` itself** —
`BOARD=beagley-ai ./build.sh [target]` (defaults to `beagleplay-ti` /
`tisdk/build` when `BOARD` is unset, unchanged from before). What `build.sh`
automates on a cold volume: copies `tisdk/build/conf/{local.conf,bblayers.conf}`
into a fresh `tisdk/build-beagley-ai/conf/`, flips `MACHINE`, comments out the
`ti-falcon` `DISTROOVERRIDES` line, and points `SSTATE_DIR` at the shared cache
— exactly the manual recipe above, made idempotent (a no-op once
`conf/local.conf` already exists). Both `meta-ultima-*` layers are synced into
`tisdk/sources/` on every run regardless of `BOARD`. **Deliberately NOT
automated**: adding either `meta-ultima-*` layer to a build dir's `BBLAYERS` —
same as this project's existing pattern for `meta-ultima-beagleplay`, that's a
one-time, human-reviewed edit to a shared config, not mechanical setup; a
truly fresh volume gets a clear "add it manually once" error naming the exact
file, and `build.sh` verifies the line is present before invoking bitbake
rather than assuming.

**Also fixed in the process**: the sync step's `cp -a` of the host layer
directories into the container was vulnerable to the same SMB
`.smbdeleteAAA*` ghost-file race documented earlier in this section (a
rename-then-unlink artifact from macOS smbfs, left behind by the
`recipes-tisdk/` deletion during the layer split — several more turned up
scattered across `meta-ultima-beagleplay-src` from the same session's churn,
all already covered by the existing `.smbdelete*` gitignore pattern but still
breaking a bind-mounted container's directory traversal). This was a
pre-existing latent bug in `build.sh`'s original single-layer sync, not
something the beagley-ai addition introduced — it just hadn't been hit
recently. Fixed for both layers by staging through `rsync -a
--exclude='.smbdelete*'` to a `mktemp -d` scratch directory (cleaned up via
`trap ... EXIT`) before bind-mounting, same workaround already used by hand
during the layer-split resync earlier in this section.

Verified: `BOARD=beagley-ai ./build.sh tisdk-base-image` run for real against
the live volume — sync + bootstrap succeeded, bitbake ran against
`tisdk/build-beagley-ai` (confirmed via its own `prserv.sqlite3`/cache paths in
the log), and the full image build completed clean (8918/8918 tasks, all
succeeded — mostly served from the sstate already warmed by this session's
earlier ad-hoc build, but end-to-end through the script this time, not manual
`docker run` invocations). Default `BOARD` (beagleplay-ti) path re-verified
too, both syncs also independently confirmed to no longer trip the ghost-file
bug.

### Ported into the new `beagley-ai/meta-ultima-beagley-ai-src` layer (`meta-ultima-beagleplay-src` left untouched)

- `IMAGE_INSTALL`: `ultima-app ultima-splash can-utils mmc-utils
  ultima-data-mount volatile-binds` + the GPU smoke-test set
  (`ti-img-rogue-driver ti-img-rogue-umlibs kmscube mesa-demos`). **Not**
  ported: `ultima-hwclock-load` (hardcodes `/dev/rtc0` = the BeaglePlay-only
  BQ32002 — this board has a **different, but real, onboard RTC**: official
  BeagleBoard docs confirm a populated **DS1340** chip with a 2-pin JST SH
  connector for an external coin-cell backup battery, same pattern as
  BeaglePlay's BQ32002/CR1220 — see
  <https://docs.beagleboard.org/boards/beagley/ai/demos/using-rtc.html>. Not
  "no RTC" — just needs its own hwclock-load variant once RTC bring-up is
  scheduled, same reasoning as the WiFi/camera skips below: this milestone's
  script hardcodes the wrong chip, not a missing one. Don't confuse this with
  the *separate*, genuinely optional `k3-am67a-beagley-ai-i2c1-rtc-rv3028.dtbo`
  overlay also in this board's `KERNEL_DEVICETREE` list — that's an add-on RTC
  module overlay (different chip, RV-3028, presumably a pluggable I2C1
  module), unrelated to the built-in DS1340 and not needed for it.), WiFi
  (`wpa-supplicant`/`wl18xx-firmware`, WL1807 is BeaglePlay-only hardware),
  `mycam004m` (camera port is a later milestone).
- `WKS_FILE:beagley-ai = "ultima-beagley-ai.wks.in"` — straight copy of
  `ultima-beagleplay.wks.in`; every line in that file is generic TI/EFI wic
  syntax (`${TI_WKS_BOOTLOADER_APPEND}`, `${EFI_PROVIDER}`, `bootimg-efi`
  source), nothing AM625-specific. **Not yet hardware-verified** that the ROM
  finds `tiboot3.bin` in p1 the same way on this board's HS-FS boot chain — the
  original beagleplay wks file has a hardware-corrected history on exactly this
  point (see its own header comment), worth re-checking here once a board
  exists rather than assuming the same result.
- `hostname:beagley-ai = "ultimagc-beagley"` — deliberately distinct from
  `beagleplay-ti`'s `ultimagc`, both machines may coexist on the network during
  bring-up and this project already has one documented same-hostname collision
  scare (see "SSH" section, eMMC vs SD both `beagleplay-ti.local`).
- `PACKAGECONFIG:append:beagley-ai = " linuxfb"` on qtbase — same reasoning as
  the beagleplay-ti line: meta-ti's `PREFERRED_PROVIDER_virtual/gpudriver`
  wiring for this machine (`j722s.inc` + `beagle-bsp.inc`, same Rogue-driver
  mechanism as beagleplay-ti.conf) already makes eglfs the default
  `PACKAGECONFIG_GL`; linuxfb added as a same-image fallback, selected via env
  var not rebuild.
- **`ultima-app.service`: linuxfb-first for this board specifically**, not
  eglfs. New file at
  `recipes-ultima/ultima-app/ultima-app/beagley-ai/ultima-app.service` in the
  new layer, plus a sibling `recipes-ultima/ultima-app/ultima-app.bbappend`
  with an explicit `FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"` — this
  file lives in a *different* layer than `ultima-app.bb` itself, so (unlike
  the single-layer version of this same idea, see the tisdk-uenv gotcha
  above) it needs that line to be found at all; it does not just fall out of
  `files/` auto-discovery the way a same-layer override would. Deliberate:
  the BXS-4-64 GPU driver stack was completely unproven on this board going
  into this milestone. Turned out **`ti-img-rogue-driver`/`ti-img-rogue-umlibs`
  build and package cleanly against the bb.org 6.12 kernel** (confirmed —
  `do_rootfs` resolved every RDEPENDS with zero GPU-related failures) — so the
  named build-time risk didn't materialize. What's still unverified is
  *runtime*: whether the driver actually initializes against real BXS-4-64
  silicon and Qt's `eglfs_kms` finds `/dev/dri/card0` — that's exactly why
  linuxfb-first stays the right call for the *first hardware boot*, even though
  the build-time risk is now cleared. Once linuxfb is confirmed rendering on
  real hardware, flip both `Environment=` lines back to
  `QT_QPA_PLATFORM=eglfs` + `QT_QPA_EGLFS_INTEGRATION=eglfs_kms` (both
  together — qtbase is built `-opengl es2`, so linuxfb alone leaves Quick
  trying the GL scenegraph with no context, same trap the beagleplay-ti file's
  own comment documents).

### Verification (build-only, no board yet)

- `bitbake tisdk-base-image` for `MACHINE=beagley-ai`: all tasks succeeded
  (8918 attempted, 1 harmless QA warning — a qtdeclarative-dev header
  referencing `TMPDIR`, pre-existing upstream noise unrelated to this port).
- Package manifest confirmed: `ultima-app`, `ultima-splash`,
  `ultima-data-mount`, `can-utils`, `mmc-utils`, `volatile-binds`,
  `ti-img-rogue-driver`, `ti-img-rogue-umlibs`, `kmscube`, `mesa-demos` all
  present for `beagley_ai`/`aarch64`.
- `.wic` partition table confirmed (decompressed + `parted print`): p1 FAT16
  134MB (boot, active), p2 ext4 1400MB (root), p3 ext4 16.8MB (`/data`) — the
  intended 3-partition layout, not the vendor-default 2-partition one, so the
  custom WKS took effect correctly.
- Full boot artifact set present in `deploy-ti/images/beagley-ai/`: R5 SPL
  (`tiboot3-j722s-hs-fs-evm...bin`), `tispl.bin`, `u-boot.img`/`u-boot-spl.bin`,
  kernel `Image`/FIT image, `k3-am67a-beagley-ai.dtb` + the full set of
  meta-beagle's overlays (i2c/PWM/PPS/mikroBUS/etc.), GRUB EFI
  (`grub-efi-bootaa64.efi`).

### Flashed to SD (2026-08-29)

Pulled `tisdk-base-image-beagley-ai.rootfs.wic.xz` (+ `.wic.bmap`) out of the
volume the same way as beagleplay-ti (see "Build environment" above), into a
new `beagleplay-falcon/deploy-beagley-ai/` (gitignored, same pattern as
`deploy-falcon/`), then flashed with `flash.sh` to the Mac's built-in SDXC
reader (`/dev/disk4` this session — showed as "internal, physical" per that
script's own header comment about the reader, confirmed removable via
`diskutil info`).

**Found before flashing, and it mattered**: `flash.sh`'s disk-signature patch
(the `deadbee5` MBR rewrite, see its own comments) is **BeaglePlay/Falcon-only
and must be skipped for beagley-ai** — new `SKIP_SIG_PATCH=1` env var added to
`flash.sh` for this. Checked by extracting p1 from the built `.wic` (`parted`
for offsets, `mtools`/`mcopy` to read the FAT partition without needing a loop
device — Docker Desktop/OrbStack on this Mac refused `losetup` even
`--privileged`, "Permission denied"/"cannot find an unused loop device").
`EFI/BOOT/grub.cfg` bakes in a **static** `root=PARTUUID=076c4a2a-02` at wic
build time:
```
menuentry 'boot'{
linux /Image root=PARTUUID=076c4a2a-02 rootwait rootfstype=ext4  ro
}
```
BeaglePlay's Falcon path re-derives this from the live partition table at
boot (`k3_falcon_fdt_fixup()`) so patching the flashed card's MBR signature is
safe there — beagley-ai has no Falcon at all, so patching the signature here
would desync it from that static baked-in PARTUUID and the kernel would fail
to find root. Moot anyway: this board has no onboard eMMC (see "Storage"
below), so the SD-vs-eMMC PARTUUID collision the patch exists to prevent
doesn't apply to it in the first place.

Not yet done: actually booting it. Next real step is serial console + power-on
verification, per this project's usual first-boot practice (see the top-level
hardware caveats in `CLAUDE.md` re: USR-button SD-boot timing).

### Falcon boot mode for BeagleY-AI — feasibility check (2026-08-28, later same day)

Went looking at what a Falcon fork for j722s would actually involve, before
attempting it. Conclusion: **this is a materially bigger job than the
BeaglePlay Falcon port was, not attempted this session, and needs a scope/
priority call from the user before someone sinks real hours into it.**

**The gap is a missing feature, not a missing wire-up.** BeaglePlay's Falcon
port (see "Fix: custom layer `meta-falcon-beagleplay`" above) worked because
`u-boot-ti-staging` already had a *complete* Falcon implementation in
`arch/arm/mach-k3/common.c` — `spl_start_uboot()`, `k3_falcon_prep()`,
`k3_falcon_fdt_fixup()` (confirmed present, lines 581–710 of that file in the
2025.01+git tree built for `beagleplay-ti`) — just never wired up for a
non-TI-EVM board name. The BeaglePlay fork was three small patches plus a
`UBOOT_CONFIG_FRAGMENTS` line scoping an *existing* TI feature to a new board.

BeagleY-AI's bootloader is different at the recipe level:
`beagle-bsp.inc`'s `bb_org` BSP variant builds `u-boot-bb.org`
(`git://github.com/beagleboard/u-boot.git`, BeagleBoard.org's own
independently-maintained fork, U-Boot 2025.10), not `u-boot-ti-staging`
(`git.ti.com/git/ti-u-boot/ti-u-boot.git`, TI's own fork, U-Boot 2025.01) —
confirmed via each recipe's `SRC_URI`/`UBOOT_GIT_URI`. Grepping the actual
unpacked `u-boot-bb.org` 2025.10 source tree built for `beagley-ai` this
session: **zero occurrences of `falcon` anywhere under `arch/arm/mach-k3`**,
and zero occurrences of `spl_start_uboot`/`CONFIG_SPL_OS_BOOT`/`k3_falcon` in
its `mach-k3/common.c` — the entire Falcon SPL subsystem TI's fork carries is
simply absent, not just unreferenced by a board config. What's established:
`k3_falcon_*` is a TI-downstream-only feature — present in TI's own
`ti-u-boot`, absent from bb.org's *newer* (2025.10 vs 2025.01) tree, so it
isn't upstream code bb.org would have inherited for free. What's genuinely
**not** established: how much `mach-k3/common.c` has otherwise diverged
between the two trees — bb.org's `mach-k3` is itself clearly TI-derived K3
support (`j722s/`, `sysfw-loader.h`, `security.c`, the same HS-FS machinery),
so this may be closer to a conflict-heavy cherry-pick than a from-scratch
transplant, or it may not — that divergence is unmeasured and is the first
thing whoever picks this up should check, not something to assume either way.
Either way it's transplanting TI's downstream `k3_falcon_*` C code (plus its
Kconfig options, the `k3_r5_falcon.config` fragment, and the `tifalcon.bin`
binman node) onto a tree that has evolved separately for ~9 months of
upstream U-Boot, with no guarantee the functions Falcon hooks into still
match shape.

**Compounded by HS-FS.** `beagley-ai-k3r5.conf` builds the HS-FS (not GP)
signed boot chain (see "The R5/TIFS boot chain is HS-FS" above) — genuinely
new territory for this project. Falcon means rebuilding the R5 SPL with a
different config fragment; whether meta-beagle's default/dev signing still
produces a chain the ROM accepts once that SPL is Falcon-modified is an open
question with no way to check without deep TI secure-boot documentation
reading, let alone hardware.

**Not attempted, and shouldn't be, without a board.** Even a syntactically
clean patch set here would be unverifiable — Falcon bugs manifest as a
completely dead boot (no U-Boot console, no kernel log), and this project has
already hit one real eMMC-corruption scare from a *known-working* config
misstep (see "eMMC persistence" above); doing first-time HS-FS + Falcon
debugging blind, with no serial-console ground truth to fall back on, is a
bad risk trade until hardware is in hand anyway.

**Recommendation, not a decision made here:** Falcon is a boot-time
optimization — valuable, but the actual stated reason for this whole
BeagleY-AI port is the Wave5 hardware encoder for dashcam recording (see
`docs/BEAGLEY-AI-EVAL.md`), which doesn't need Falcon at all. Given a board
isn't even in hand yet, the camera driver port and CAN bring-up (both also
deferred, see below) deliver more toward that actual goal per hour spent than
a from-scratch Falcon backport with no way to verify it. Suggest deferring
Falcon until after first hardware boot (stock, non-Falcon) is confirmed
working, and revisiting priority then — flagging it here rather than
defaulting to "keep going."

### Deferred (not this milestone)

Falcon boot (see feasibility check just above — bigger than expected, defer/
proceed is a call for the user), the MY-CAM004M camera driver port (open
question: J722S CSI-2 receiver multi-virtual-channel demux for 4 simultaneous
1080p streams — see `docs/BEAGLEY-AI-EVAL.md`'s capture-risk section, same
open question flagged there for this exact board), CAN bring-up (Waveshare
MCP2515 HAT on the real MCU_SPI0, not the header's default software-SPI shim
— see `docs/BEAGLEY-AI-EVAL.md`'s CAN section), WiFi, RTC, and — obviously —
anything requiring the physical board: flashing, serial verification, and the
eglfs switch-back.
