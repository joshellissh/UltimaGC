# Ultima

Minimal Buildroot-based Linux image for Raspberry Pi 5 that boots directly into a
fullscreen Qt5/QML gauge cluster, fed live data from a car's CAN bus.

**Read `SETUP.md` first** for anything touching the build, boot sequence, kernel
config, or Qt app structure — it's a complete, maintained reproduction guide (host
setup, VM setup, defconfig, kernel fragments, init scripts, CAN bus integration,
flashing, EEPROM, debugging). This file only covers what isn't obvious from reading
that guide or the code.

## Environment

- Build happens on an OrbStack Ubuntu VM (`ssh ubuntu@orb`, Buildroot cloned to
  `~/ultima/buildroot`), not on this Mac. Sync source with `rsync` before building.
- The Mac can run the Qt app natively for QML/layout iteration (`scripts/dev-build.sh`,
  Qt 6 via Homebrew) — see "Local macOS Dev Build" in `SETUP.md`. `CanBus` simulates
  driving data on non-Linux instead of reading real CAN.
- Target hardware (Pi + Syvecs S7+ ECU + CAN adapter) is not available in every
  session — don't assume you can flash or CAN-sniff live; ask before assuming access.

## Rules that are easy to get wrong

- **CAN1 is the powertrain bus on the ECU — never touch it.** Only CAN2 carries dash
  data; that's the only bus this project reads or writes.
- The bundled `.dbc` file describes CAN1's fixed stream, not this car's CAN2 layout.
  Use it only for per-channel scaling/signedness lookups, never for frame IDs.
- Syvecs `.SC` config files are proprietary/encrypted — don't try to parse one to
  recover CAN Tx config. Ask the user for a SCal screenshot instead.
- Don't remove the `remount,rw` line from the inittab overlay — it looks redundant
  next to `S00remountro` but removing it breaks VC4 display init.
- Kernel `cma=` can't go below 320MB, or `raspberrypi-clk` fails to probe and VC4
  DRM breaks.
- The app launches before udev (for boot speed), so anything reading `/dev` or a
  network interface at startup (CanBus's `can0`) must retry, not assume presence.

## Workflow shortcuts

- Overlay/init script change only: `make` (no dirclean).
- App source change: `make ultima-app-dirclean && make`.
- Fast iteration without a full reflash: see "Hot-Deploy to Pi" in `SETUP.md`.
