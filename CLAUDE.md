# Ultima

Minimal Buildroot-based Linux image that boots directly into a fullscreen Qt5/QML
gauge cluster, fed live data from a car's CAN bus. Currently ships on Raspberry Pi 5;
a BeaglePlay (AM625) port is upcoming. The two targets share one Qt app
(`br2-external/package/ultima-app/`) and get their own `board/` directory, defconfig,
and setup guide apiece — see "Repo layout across board targets" below.

**Read `SETUP-RPI5.md` first** for anything touching the RPi5 build, boot sequence,
kernel config, or Qt app structure — it's a complete, maintained reproduction guide
(host setup, VM setup, defconfig, kernel fragments, init scripts, CAN bus integration,
flashing, EEPROM, debugging). This file only covers what isn't obvious from reading
that guide or the code. A `SETUP-BEAGLEPLAY.md` will exist once that port starts.

## Repo layout across board targets

- `br2-external/package/ultima-app/` — the Qt app. **Shared, board-agnostic.** No
  RPi-specific code lives here (confirmed by audit); don't fork it per board.
- `br2-external/board/ultima-rpi5/`, `br2-external/configs/ultima_rpi5_defconfig` —
  RPi5-only: boot config, kernel fragments, DT overlays, init overlay, KMS config.
  A future `board/ultima-beagleplay/` + `configs/ultima_beagleplay_defconfig` follow
  the same shape but with entirely different content (different bootloader, different
  DRM driver — nothing here transfers as-is).
- `scripts/` — `setup-vm.sh`, `dev-build.sh`, `dev-build-wsl.sh` are shared (VM deps
  and local app-only dev builds don't depend on the target board). `build-rpi5.sh`,
  `flash-rpi5.sh`, `read-logs-rpi5.sh` are RPi5-specific; BeaglePlay equivalents get
  their own `-beagleplay` scripts rather than branching inside these.

## Environment

- Build happens on an OrbStack Ubuntu VM (`ssh ubuntu@orb`, Buildroot cloned to
  `~/ultima/buildroot`), not on this Mac. Sync source with `rsync` before building.
- The Mac can run the Qt app natively for QML/layout iteration (`scripts/dev-build.sh`,
  Qt 6 via Homebrew) — see "Local macOS Dev Build" in `SETUP-RPI5.md`. `CanBus`
  simulates driving data on non-Linux instead of reading real CAN.
- Target hardware (Pi + Syvecs S7+ ECU + CAN adapter) is not available in every
  session — don't assume you can flash or CAN-sniff live; ask before assuming access.
  Same caution applies to BeaglePlay hardware once that port starts.

## Rules that are easy to get wrong

**Car / ECU — applies regardless of which board is driving the dash:**

- **CAN1 is the powertrain bus on the ECU — never touch it.** Only CAN2 carries dash
  data; that's the only bus this project reads or writes.
- The bundled `.dbc` file describes CAN1's fixed stream, not this car's CAN2 layout.
  Use it only for per-channel scaling/signedness lookups, never for frame IDs.
- Syvecs `.SC` config files are proprietary/encrypted — don't try to parse one to
  recover CAN Tx config. Ask the user for a SCal screenshot instead.
- The app launches before udev (for boot speed), so anything reading `/dev` or a
  network interface at startup (CanBus's `can0`) must retry, not assume presence.
  This is a board-agnostic app-level pattern, not RPi-specific.

**RPi5-specific (see `SETUP-RPI5.md` for full context):**

- Don't remove the `remount,rw` line from the inittab overlay — it looks redundant
  next to `S00remountro` but removing it breaks VC4 display init.
- Kernel `cma=` can't go below 320MB, or `raspberrypi-clk` fails to probe and VC4
  DRM breaks.

## Workflow shortcuts

- Overlay/init script change only: `make` (no dirclean).
- App source change: `make ultima-app-dirclean && make`.
- Fast iteration without a full reflash: see "Hot-Deploy to Pi" in `SETUP-RPI5.md`.
