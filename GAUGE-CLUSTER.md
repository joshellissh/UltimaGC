# Ultima Gauge Cluster — Qt App Structure & CAN Bus Integration

Board-agnostic reference for the Qt5/QML app itself (`ultima-app/`)
and the live CAN data it displays. This content applies regardless of which board is
running the dash. For board-specific build/boot/flash instructions, see
`beagleplay-falcon/NOTES.md`.

## Qt App Structure

### Source Files

All in `ultima-app/src/`. Don't copy these listings verbatim
elsewhere — read `ultima-app.pro` / `qml.qrc` directly for the current file set; they
change as the app evolves and a stale copy is worse than no copy.

- **`ultima-app.pro`** — qmake project file: `HEADERS`/`SOURCES` list `odostore`,
  `canbus`, `systemclock`; `RESOURCES += qml.qrc`. A `ultima_dev_sim` CONFIG flag
  (set by `scripts/dev-build-wsl.sh`) defines `ULTIMA_SIMULATE` to force simulated
  gauge data on a Linux dev build too — macOS dev builds simulate unconditionally
  regardless of this flag (see `canbus.h`/`.cpp` below).
- **`main.cpp`** — creates `OdoStore` for persistent odometer, `CanBus` as the live
  gauge data source, and `SystemClock` for the time-set screen; exposes all three to
  QML as context properties (`odoStore`, `sim`, `systemClock`), plus a `bootTime`
  timestamp used for startup-latency logging (`fprintf(stderr, ...)` at each
  init stage, read from `/proc/uptime`). Saves odometer state on SIGTERM/SIGINT via
  `CanBus::save()` (falls back to `OdoStore::save()` directly if `CanBus` isn't up
  yet).
- **`odostore.h` / `odostore.cpp`** — `OdoStore` is a `QObject` with `totalOdo` and
  `tripOdo` properties. Reads `/data/odometer.json` on construction (defaults to
  2347.0 / 0.0 if missing). `save()` slot writes JSON. `main.qml`'s periodic save
  timer calls `sim.save()`, which is `CanBus::save()` — it flushes into `OdoStore`
  before persisting (see below).
- **`canbus.h` / `canbus.cpp`** — `CanBus` is the live gauge data source, exposed to
  QML as the `sim` context property (a drop-in replacement for `SimEngine.qml`'s
  property set, so `main.qml` didn't need to change). Full details — hardware, frame
  map, debugging — are in [CAN Bus Integration](#can-bus-integration-syvecs-s7)
  below. Summary: on Linux it opens a raw `PF_CAN` socket on `can0`, retrying every
  1s until the interface exists, and decodes Syvecs S7+ CAN2 frames via a
  `QSocketNotifier`. On non-Linux (macOS dev builds) or when built with
  `CONFIG+=ultima_dev_sim`, `simulateTick()` — a `QTimer`-driven simulator —
  reproduces `SimEngine.qml`'s phase-based driving profile (city/stop/suburban/
  highway/spirited legs) directly on `CanBus`'s member state, so the gauges animate
  without real CAN hardware. This is what backs the local macOS dev build (see
  [Local macOS Dev Build](#local-macos-dev-build-with-simulated-can-data) and
  `scripts/dev-build.sh`).
- **`systemclock.h` / `systemclock.cpp`** — lets the QML time-set screen
  (`SetTimeScreen.qml`) push a new wall-clock time to the kernel via
  `clock_settime()`, with a best-effort write-through to a battery-backed hardware
  RTC at `/dev/rtc0` if one is present (BeaglePlay has one, onboard BQ32002). No-op
  on non-Linux dev builds.

### QML Files

The app consists of 4 QML files, all in `ultima-app/src/`:

- **`main.qml`** — Root layout: 4 gauge needles over the Bavarian background layers, boost gauge (trapezoid black overlay with PSI readout), turn signal indicators, top indicator row (oil, check engine, beams, battery, coolant — warn icons flash at 300ms), gear indicator (Bahnschrift SemiBold font, P/R/N/1-7), odometer + trip odometer with reset button, touch feedback dot. On startup, a ~2s self-test (`startupActive`/`startupFrac`/`startupFlash`) sweeps every needle to max and back and flashes every icon before handing off to live `sim.*` values — real gauge/warning bindings are `startupActive ? <test value> : sim.<prop>`. Includes a periodic (30s) save timer for odometer persistence via `sim.save()`. All gauge values bind to the `sim` context property, which is the `CanBus` C++ object, not this file's `SimEngine.qml`. Hosts `SetTimeScreen` for clock adjustment.
- **`CircularGauge.qml`** — Reusable needle gauge component: rotates `needle.png` over a transparent item positioned at the gauge center. Configurable start/end angles, counter-clockwise mode, needle size/pivot, optional debug arc overlay
- **`SetTimeScreen.qml`** — Time-set overlay that calls into `SystemClock` (the `systemClock` context property) to push a new wall-clock time
- **`SimEngine.qml`** — **Not loaded at runtime.** Bundled only as a reference/potential `--demo` fallback; it predates `CanBus` and was the original simulated data source before real CAN integration. Speed wanders through city/suburban/highway/stop phases, RPM derived from gear ratios, automatic gear selection (P/R/N/1-7), fuel consumption, coolant temp, boost pressure (0-30 PSI). `CanBus::simulateTick()` reimplements this same phase logic directly in C++ so it can drive the live `sim` property — see [CAN Bus Integration](#can-bus-integration-syvecs-s7)

### Asset Files

`main.qml`'s background uses the "Bavarian" theme (source PSDs/exports in
`Ultima Gauge Cluster/Bavarian/` outside this repo): 4 stacked full-canvas
(1600x720) layers instead of one flat image — back to front:

| File | Purpose |
|------|---------|
| `boost_circle.png` | Backmost layer — boost gauge dial (opaque, own black backing) |
| `background_overlay.png` | Gauge/dial face overlay (tick marks, TOTAL/TRIP text area, mini fuel/coolant arcs; alpha) |
| `car_lights_off.png` | Centered car render (alpha) |
| `car_lights_on.png` | Same, headlights lit — `visible: sim.lowBeams \|\| sim.highBeams`, loaded `asynchronous: true` so the off-state boot path isn't blocked decoding it |
| `needle.png` | Gauge needle image (rotated by CircularGauge) |
| `left_indicator.png` | Turn signal arrow icon (mirrored for right) |
| `range.regular.ttf` | Font for odometer and boost PSI display |
| `bahnschrift._semibold.ttf` | Bahnschrift SemiBold font for gear indicator |
| `icon_oil_pressure.png`, `icon_check_engine.png`, `icon_battery.png`, `icon_coolant_warn.png`, `icon_low_beam.png`, `icon_high_beam.png`, `icon_axle_lift.png`, `icon_cruise.png` | ISO 7000-style dashboard warning icons (white on transparent) |

The old single-image `background.png` face has been removed from the tree and
`qml.qrc` — the Bavarian swap described above is complete, not in progress.

### Gauge Needle Alignment

The per-gauge pivot/angle constants live inline in each `CircularGauge {}` block in
`main.qml` — read them there, not here. A prior version of this doc carried a
snapshot table, but it was calibrated against the pre-Bavarian `background.png` dial
art (different tick layout/arc extents) and had already been flagged stale; a copy
here would just drift again. Pull the old numbers from git history if useful as a
rough starting point when re-measuring.

### Local macOS Dev Build (with Simulated CAN Data)

For iterating on QML/layout without a board or CAN hardware, build and run the app natively on macOS with desktop Qt:

```bash
brew install qt   # Qt 6 via Homebrew — the .pro file has no Qt5-specific dependencies, builds fine under Qt6
scripts/dev-build.sh   # qmake6 + make in ./build/, then opens ultima-app.app
```

`CanBus` is written so the Linux-only `#ifdef __linux__` block (SocketCAN: `socket(PF_CAN, ...)`, `linux/can.h`, etc.) compiles out entirely on macOS. In its place, `CanBus::tryConnect()`'s `#else` branch starts a `QTimer`-driven `simulateTick()` that generates a realistic driving profile (see [CAN Bus Integration](#can-bus-integration-syvecs-s7) below), so the gauges animate immediately without any hardware. `main.cpp`'s `/proc/uptime` read and `OdoStore`'s `/data/odometer.json` path both fail open (silently) when missing on macOS, so nothing else needs stubbing.

WSL2 has an equivalent script, `scripts/dev-build-wsl.sh` — see the comment at the top of that file for setup.

## CAN Bus Integration (Syvecs S7+)

The dash gets live engine/vehicle data from the car's Syvecs S7+ ECU over CAN, decoded by `CanBus` (`canbus.h`/`canbus.cpp`) and exposed to QML as the `sim` context property.

### Hardware

- **Adapter**: ODrive USB-CAN adapter (`gs_usb`/candleLight class, VID:PID `1d50:606f`). Plugs into a USB port on whichever board is running the dash; screw terminals wire to the ECU's **B2 (CAN_H) / B3 (CAN_L)** pins.
- **Termination**: the adapter has a switchable 120 Ω terminator built in. The Syvecs S7+'s CAN2 bus has **no on-board termination**, so an external 120 Ω resistor must be placed across CAN_H/CAN_L at the ECU end, or the bus won't terminate correctly.
- **Kernel support**: `CONFIG_CAN`, `CAN_RAW`, `CAN_GS_USB` need to be enabled (BeaglePlay: `beagleplay-falcon/meta-ultima-beagleplay-src/recipes-kernel/linux/linux-ti-staging/ultima-can.cfg`).
- **Bring-up**: a udev rule matches the adapter's VID:PID and runs `ip link set can0 type can bitrate 1000000 && ip link set up can0` automatically on plug-in (BeaglePlay: `beagleplay-falcon/meta-ultima-beagleplay-src/recipes-ultima/ultima-app/files/70-can.rules`). `CanBus` doesn't assume `can0` exists at startup regardless — it retries `socket(PF_CAN)` + `bind()` every 1s until the interface appears, so app start never has to race udev.

### ECU Configuration

The ECU is a Syvecs S7+ on a Cayman LS7 engine swap (firmware string `S7Plus 1.778.1- LS7SC - Cayman Standalone`), configured via Syvecs' SCal tool.

- **CAN1 is the powertrain bus — never touch it.** All dash data comes from **CAN2, running at 1 Mbit/s** (verified in SCal: Datastreams → CAN2 Bus Speed).
- CAN2's outbound frames are configured via SCal **Datastreams → Generic CAN Transmit**. This build does *not* have the "Custom CAN" / "Datastream Select" hierarchy that some gaugeART documentation references — don't assume that structure applies here.
- **Syvecs `.SC` config files are proprietary binary** (encrypted/packed after a small header) — there's no way to extract the CAN Tx config by inspecting an `.SC` file directly. If you need to verify or change the CAN2 layout, ask for a screenshot of the SCal Datastreams → Generic CAN Transmit screen rather than trying to parse a config file.

### DBC Reference (Scalings Only — Not Frame IDs)

`ultima-app/can/syvecs_s7_fixed_stream_v3.dbc` is bundled as a reference, but it describes the **CAN1 fixed stream**, not this car's CAN2 layout. Its per-channel scaling and signedness definitions are tied to channel name (e.g. `vehicleSpeed`'s `0.036` km/h scaling), which holds true regardless of which frame/slot a channel is transmitted on — so it's useful for looking up scaling factors, but **do not use it to look up frame IDs** for CAN2.

### Verified CAN2 Frame Map

Read from SCal Datastreams → Generic CAN Transmit → Transmit Content. Frame *N* broadcasts on CAN ID `0x600 + (N-1)`; each frame carries four 16-bit big-endian slots, slot *S* at byte offset `2(S-1)..2S-1`. What `CanBus::decodeFrame()` consumes today:

| Frame (CAN ID) | Bytes | Channel | Decoding |
|---|---|---|---|
| `0x600` | 0-1 | rpm | signed, clamped ≥ 0 |
| `0x604` | 6-7 | limpMode | unsigned enum; non-zero → `checkEngine` |
| `0x605` | 2-3 | ect1 (coolant) | raw × 0.18 + 32 → °F; `coolantWarn` if > 220 °F |
| `0x608` | 0-1 | eop1 (oil pressure) | raw × 0.0145038 → psi; `oilPressureWarn` if rpm ≥ 600 && psi ≤ 40 |
| `0x60E` | 2-3 | gear | Syvecs enum 0=Unknown 1=R 2=N 3..10=1st..8th → QML -1/0/1..8 |
| `0x60E` | 4-5 | vbat | V × 0.001 (unsigned); `batteryWarn` if v < 12.5 |
| `0x60F` | 0-1 | vehicleSpeed | raw × 0.0223694 → mph; drives odometer accumulation |

**Not on CAN2 in the current SCal config:**
- `flvlA` (fuel level) — the fuel gauge stays at 0 until this channel is added to SCal's Transmit Content.
- `sensorWarningLevel` — `checkEngine` currently derives from `limpMode` alone.
- `mapMax` (boost, `0x614`/frame 21) — previously documented here as verified and wired to the boost gauge, but the xlsx built from `CAN2.png` (the source of truth for this mapping) shows frame 21 as all SPARE. Decode removed from `CanBus::decodeFrame()` pending re-verification against a current SCal Datastreams screenshot; boost gauge reads 0 until then.

**Not on CAN at all** (no Syvecs channels exist for these): `leftIndicator`, `rightIndicator`, `axleLift` are `CONSTANT`/`false` member properties on `CanBus`, present purely so `main.qml`'s bindings still compile. `lowBeams`, `highBeams`, `cruiseControl`, `transmissionAuto`, `driveMode` are real READ+NOTIFY properties, but on real hardware they're only ever set to their defaults (`false`/`false`/`false`/`true`/`"SPORT"`) — nothing decodes them from a CAN frame. The dev-build simulator (`simulateTick()`) is the only thing that ever changes them, for layout review.

### Debugging

- `candump can0` (can-utils is included in the image) to watch raw traffic.
- `ip -details link show can0` to check bitrate/link state.
- `CanBus` logs `[canbus] ...` lines to stderr on connect/bind failures and reconnects — check `journalctl -u ultima-app` (BeaglePlay logs to journald, see `ultima-app.service`).
