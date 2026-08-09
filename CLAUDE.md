# Ultima

Minimal Linux image that boots directly into a fullscreen Qt5/QML gauge cluster, fed
live data from a car's CAN bus. Ships on Raspberry Pi 5, with a BeaglePlay (AM625) port
that's hardware-verified and running.

**The two boards use two entirely different build systems — this split is
permanent, not incidental:**

- **RPi5 → Buildroot.** `br2-external/`, `SETUP-RPI5.md`. Mainline U-Boot has
  everything this board needs.
- **BeaglePlay → Yocto/TI SDK.** `beagleplay-falcon/`, `beagleplay-falcon/NOTES.md`.
  Not a Buildroot target and never has been as a *working* path — a Buildroot
  BeaglePlay board config existed early on but was scaffolded without hardware in
  hand, got hardware-verified once, and was then deleted once the Yocto path proved
  out end-to-end (2026-08-08) and made it redundant. The reason for the split is real,
  not historical accident: BeaglePlay's TI Falcon boot mode (R5 SPL jumping straight
  to the kernel, skipping A53 SPL/U-Boot-proper/GRUB — the biggest boot-time win
  found on this board) needs logic that only exists in TI's downstream `ti-u-boot`
  fork, not in the mainline U-Boot Buildroot pins. **So: if the conversation is about
  BeaglePlay, assume Yocto and look in `beagleplay-falcon/`. If it's about RPi5,
  assume Buildroot.** Don't go looking for a `br2-external/board/ultima-beagleplay/`
  or an `ultima_beagleplay_defconfig` — they don't exist anymore.

Both boards run the same Qt app source (`br2-external/package/ultima-app/`,
board-agnostic) — Buildroot builds it directly as a package; the Yocto side
bind-mounts that same source directory in and builds it via its own recipe
(`beagleplay-falcon/meta-ultima-beagleplay-src/`). One app, two unrelated build
pipelines around it.

**Read `SETUP-RPI5.md` first** for anything touching the RPi5 build, boot sequence,
kernel config, or Qt app structure — it's a complete, maintained reproduction guide
(host setup, VM setup, defconfig, kernel fragments, init scripts, CAN bus integration,
flashing, EEPROM, debugging). This file only covers what isn't obvious from reading
that guide or the code. **Read `beagleplay-falcon/NOTES.md` first** for the BeaglePlay
port — it's the equivalent reproduction/status doc for the Yocto build, including
hardware gotchas (boot-mode switch timing, a kernel-module-autoload bug that looked
like an app crash, an SD-card write-error scare from a crash-loop) worth knowing
before touching that board again.

## Repo layout

- `br2-external/package/ultima-app/` — the Qt app source. **Shared, board-agnostic.**
  No RPi-specific code lives here (confirmed by audit); don't fork it per board.
- `br2-external/board/ultima-rpi5/`, `br2-external/configs/ultima_rpi5_defconfig` —
  RPi5's Buildroot board config: boot config, kernel fragments, DT overlays, init
  overlay, KMS config.
- `scripts/` — `setup-vm.sh`, `dev-build.sh`, `dev-build-wsl.sh` are shared (VM deps
  and local app-only dev builds don't depend on the target board). `build-rpi5.sh`,
  `flash-rpi5.sh`, `read-logs-rpi5.sh` are RPi5/Buildroot-specific.
- `beagleplay-falcon/` — the whole BeaglePlay side: Yocto/TI-SDK (`tisdk`,
  Docker-based, runs via local Docker/OrbStack on the Mac, not the `ssh ubuntu@orb`
  VM Buildroot uses) build producing TI Falcon boot mode plus `ultima-app` on top of
  it. `meta-ultima-beagleplay-src/` is the Yocto layer with the app recipe, Qt5/
  `linuxfb` packaging, and kernel fragments; `meta-falcon-beagleplay-src/` is the
  separate layer that wires up TI's falcon-boot logic (kept apart from the app
  layer — unrelated concerns). See `beagleplay-falcon/NOTES.md` for the full story.

## Environment

- **RPi5/Buildroot** build happens on an OrbStack Ubuntu VM (`ssh ubuntu@orb`,
  Buildroot cloned to `~/ultima/buildroot`), not on this Mac. Sync source with
  `rsync` before building.
- **BeaglePlay/Yocto** build happens via local Docker (OrbStack) directly on this
  Mac, not the VM — `beagleplay-falcon/build.sh` / `run.sh`. Build state lives in a
  Docker-managed volume (`falcon-yocto-build`), not a bind mount — see
  `beagleplay-falcon/NOTES.md` for why.
- The Mac can run the Qt app natively for QML/layout iteration (`scripts/dev-build.sh`,
  Qt 6 via Homebrew) — see "Local macOS Dev Build" in `SETUP-RPI5.md`. `CanBus`
  simulates driving data on non-Linux instead of reading real CAN.
- Target hardware (Pi or BeaglePlay + Syvecs S7+ ECU + CAN adapter) is not available
  in every session — don't assume you can flash or CAN-sniff live; ask before
  assuming access.

## Rules that are easy to get wrong

**Car / ECU — applies regardless of which board is driving the dash:**

- **CAN1 is the powertrain bus on the ECU — never touch it.** Only CAN2 carries dash
  data; that's the only bus this project reads or writes.
- The bundled `.dbc` file describes CAN1's fixed stream, not this car's CAN2 layout.
  Use it only for per-channel scaling/signedness lookups, never for frame IDs.
- Syvecs `.SC` config files are proprietary/encrypted — don't try to parse one to
  recover CAN Tx config. Ask the user for a SCal screenshot instead.

**RPi5/Buildroot-specific (see `SETUP-RPI5.md` for full context):**

- Don't remove the `remount,rw` line from the inittab overlay — it looks redundant
  next to `S00remountro` but removing it breaks VC4 display init.
- Kernel `cma=` can't go below 320MB, or `raspberrypi-clk` fails to probe and VC4
  DRM breaks.
- The app launches before udev (for boot speed), so anything reading `/dev` or a
  network interface at startup (CanBus's `can0`) must retry, not assume presence.
  **This is a Buildroot-only pattern** — the Yocto/BeaglePlay build boots normally
  under systemd/udev and does not do this; don't port this assumption over.

**BeaglePlay/Yocto-specific (see `beagleplay-falcon/NOTES.md` for full context):**

- Holding **USR** forces SD boot over eMMC — but it must be held *before* power/reset
  is applied and kept down for a couple seconds after, not tapped once boot is
  already underway. Tapping it late silently boots stale eMMC content instead, which
  looks like a real failure (hit `emergency mode` twice this way) but has nothing to
  do with anything in this repo.
- `CONFIG_DRM_TIDSS=y` gets silently downgraded to `=m` by a Kconfig dependency
  (not root-caused) — don't trust a config fragment requesting `=y` actually landed
  without checking the built `.config`. The fix in place is forcing the module to
  load via `/etc/modules-load.d/tidss.conf` rather than relying on udev coldplug,
  which real hardware showed never actually loading it.
- **This rootfs is not read-only.** If `ultima-app` (or anything else) starts
  crash-looping, power the board off immediately rather than leave it running to
  diagnose — a rapid crash-loop hammering journald/coredump writes onto a live SD
  card produced real `I/O error`s on the rootfs partition in one session. Reflash
  fresh afterward rather than trust that card's state.
- No WiFi in this build (unlike the Buildroot boards) — only wired Ethernet gives
  SSH/dropbear a path in; otherwise it's serial-console-only.

## Workflow shortcuts

**RPi5/Buildroot:**
- Overlay/init script change only: `make` (no dirclean).
- App source change: `make ultima-app-dirclean && make`.
- Fast iteration without a full reflash: see "Hot-Deploy to Pi" in `SETUP-RPI5.md`.

**BeaglePlay/Yocto:**
- `beagleplay-falcon/build.sh [target]` syncs the Yocto layer into the build volume
  and runs bitbake — defaults to `tisdk-base-image`. See `beagleplay-falcon/NOTES.md`
  for the full build/flash/verify cycle.

## Git commit conventions

- **Never add a `Co-Authored-By: Claude ...` or `Claude-Session: ...` trailer to
  commit messages in this repo.** This overrides the default commit-message
  format Claude Code normally appends. The full existing history was rewritten
  (2026-08-04) to strip these trailers specifically so Claude doesn't show up as
  a contributor — don't reintroduce them.
