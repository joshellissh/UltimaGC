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

The app consists of 9 QML files, all in `ultima-app/src/`:

- **`main.qml`** — Root layout: 4 gauge needles over the Bavarian background layers, boost gauge (trapezoid black overlay with PSI readout), turn signal indicators, top indicator row (oil, check engine, beams, battery, coolant — warn icons flash at 300ms), gear indicator (Bahnschrift SemiBold font, P/R/N/1-7), odometer + trip odometer with reset button, touch feedback dot, 360-view tap icon. On startup, a ~2s self-test (`startupActive`/`startupFrac`/`startupFlash`) sweeps every needle to max and back and flashes every icon before handing off to live `sim.*` values — real gauge/warning bindings are `startupActive ? <test value> : sim.<prop>`. Includes a periodic (30s) save timer for odometer persistence via `sim.save()`. All gauge values bind to the `sim` context property, which is the `CanBus` C++ object, not this file's `SimEngine.qml`. Hosts `SetTimeScreen`, `DiagnosticScreen`, `CameraGridScreen`, and `Camera360Screen` as overlays; a swipe left/right on the main dash opens `DiagnosticScreen`/`CameraGridScreen` respectively.
- **`CircularGauge.qml`** — Reusable needle gauge component: rotates `needle.png` over a transparent item positioned at the gauge center. Configurable start/end angles, counter-clockwise mode, needle size/pivot, optional debug arc overlay
- **`SetTimeScreen.qml`** — Time-set overlay that calls into `SystemClock` (the `systemClock` context property) to push a new wall-clock time
- **`DiagnosticScreen.qml`** — Full-screen grid of every CAN2 signal this build actually decodes today (ECU + MCE18 — see below), showing the raw decoded value/enum behind each dash gauge or icon rather than just its needle position or lit/unlit state. Opened by swiping left anywhere on the main dash, closed by swiping right
- **`Camera360Screen.qml`** — Full-screen camera overlay, toggled open/closed (250ms cross-fade via a `Behavior on opacity`) by tapping the 360 icon (`icon360`) on the main dash, or automatically on reverse gear (`main.qml`'s `reverseGear`). Feeds the 4 raw streams from the mycam004m driver (`cameraFeed1`..`cameraFeed4` context properties set up in `main.cpp`, each a `CameraFeed` opening one of the driver's stable `/dev/mycam/cam1`..`cam4` symlinks — fake or real backend, see `~/code/mycam004m/docs/ultima-app-integration.md`) into `SurroundView` (`surroundview.h`), which stitches them into one top-down birds-eye composite via a precomputed per-camera fisheye/ground-plane warp mesh + feather blend. Calibration (per-camera position/yaw/pitch/FOV, vehicle dimensions, ground extent, wedge overlap) defaults to a placeholder rig (`cameracalibration.cpp`'s `defaultCalibration()`) but is live-tunable and persisted via the gear-icon settings panel (`CalibrationSettingsScreen.qml`) — seams/ground-plane scale are only exactly right once real measured camera-mount data replaces the placeholder. `simulated_cameras.png` is a whole-screen fallback shown only if every feed has failed or none produce a frame within 2s of opening.
- **`CameraGridScreen.qml`** — Persistent swipeable screen (mirror of `DiagnosticScreen`, reached by swiping right off the main dash) showing the same 4 mycam004m feeds as a plain 2x2 grid, one quadrant per physical camera, no stitching — a raw-feed debug view alongside `Camera360Screen`'s stitched one. Separate file/screen rather than a mode of `Camera360Screen` so that screen's opacity-fade + reverse-gear auto-open behavior stays untouched.
- **`CalibrationSettingsScreen.qml`** — Slide-in panel (opened from `Camera360Screen`'s gear icon) for live-tuning `SurroundView`'s stitching parameters; every edit writes to `CalibrationStore` (`calibrationstore.h`, persisted to `/data/calibration.json`, same load/save pattern as `OdoStore`) and re-warps the mesh immediately via `calibrationChanged()`, no restart needed. Fixed lens/render properties (image size, principal point, fisheye k1-k4, mesh grid resolution) aren't exposed here.
- **`CalibrationParamRow.qml`** — Reusable row (label, live value, -/+ steppers with press-and-hold repeat) used throughout `CalibrationSettingsScreen`; touch-friendly for the small numeric steps calibration tuning needs.
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
| `icon_360.png` | 360-view tap target, centered at (280, 666); toggles `Camera360Screen` open/closed |
| `simulated_cameras.png` | `Camera360Screen`'s whole-screen fallback art, shown when no camera feed is up (`car_360.png` is unused, kept in `qml.qrc` from an earlier version of this overlay) |

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

### Verified CAN2 Frame Map

Read from SCal Datastreams → Generic CAN Transmit → Transmit Content. Frame *N* broadcasts on CAN ID `0x600 + (N-1)`; each frame carries four 16-bit big-endian slots, slot *S* at byte offset `2(S-1)..2S-1`. What `CanBus::decodeFrame()` consumes today:

| Frame (CAN ID) | Bytes | Channel | Decoding |
|---|---|---|---|
| `0x600` | 0-1 | rpm | signed, clamped ≥ 0 |
| `0x600` | 6-7 | map1A (boost) | signed, mbar 1:1 (SCal: y=(1\*x)+0, 0..3000, Pressure/Millibar/Signed); psi = (mbar − 1013.25) × 0.0145038, clamped ≥ 0. Confirmed against SCal Datastreams screenshot 2026-08-13; not yet candump-confirmed on the wire. |
| `0x601` | 0-1 | cruiseState → `cruiseControl` | unsigned enum: 0=OFF 1=ON 2=ACTIVE; icon lit only when ACTIVE (ON reads as not-lit, same as OFF) |
| `0x604` | 6-7 | limpMode | unsigned enum; non-zero → `checkEngine` |
| `0x605` | 2-3 | ect1 (coolant) | raw × 0.18 + 32 → °F; `coolantWarn` if > 220 °F |
| `0x605` | 4-5 | ManualAuto_U12 → `transmissionAuto` | unsigned enum; nonzero → Automatic. Frame/slot per the user, not a SCal screenshot; polarity (which value means Automatic) is assumed, not confirmed either way. |
| `0x608` | 0-1 | eop1 (oil pressure) | raw × 0.0145038 → psi; `oilPressureWarn` if rpm ≥ 600 && psi ≤ 40 |
| `0x60E` | 2-3 | gear | Syvecs enum 0=Unknown 1=R 2=N 3..10=1st..8th → QML -1/0/1..8 |
| `0x60E` | 4-5 | vbat | V × 0.001 (unsigned); `batteryWarn` if v < 12.5 |
| `0x60F` | 0-1 | vehicleSpeed | raw × 0.0223694 → mph; drives odometer accumulation |

**Not on CAN2 in the current SCal config:**
- `flvlA` (fuel level) — no Syvecs channel for this; now sourced from the MCE18 expander instead (see below), not SCal.
- `sensorWarningLevel` — `checkEngine` currently derives from `limpMode` alone.
- `mapMax` (boost, `0x614`/frame 21) — previously documented here as verified and wired to the boost gauge, but the xlsx built from `CAN2.png` (the source of truth for this mapping) shows frame 21 as all SPARE. Decode removed from `CanBus::decodeFrame()` pending re-verification against a current SCal Datastreams screenshot; boost gauge reads 0 until then.

**Not on CAN at all:** `driveMode` (no Syvecs channel exists, and no physical selector is known to exist in the car) is a real READ+NOTIFY property, but on real hardware it's only ever set to its default (`"SPORT"`) — nothing decodes it from a CAN frame or an MCE18 input. It's a 3-state QString, which doesn't fit a single digital input the way the MCE18-sourced booleans below do; wiring it (e.g. to a real drive-mode selector switch) is unstarted. The dev-build simulator (`simulateTick()`) is the only thing that ever changes it, for layout review.

### MCE18 CAN Bus Expander (Unverified — Datasheet Default, Not Wire-Confirmed)

Fills part of the gap above: `flvlA` (fuel level) and five booleans that have no
Syvecs channel at all (`leftIndicator`, `rightIndicator`, `axleLift`, `lowBeams`,
`highBeams`) are read from a CANchecked-protocol MCE18 CAN bus expander instead of
the ECU. Source: "CAN2 MCE18 Mapping.pdf" (CANchecked MCE18 manual, Rev 2.0).
`transmissionAuto` and `cruiseControl` are deliberately *not* part of this — both
come from the Syvecs stream instead (`0x605` slot 3, ManualAuto_U12, and `0x601`
slot 1, cruiseState — see the frame map above).

**Everything in this section is a datasheet default, not verified against this car**
— no MCE18 unit has been on the bench or candump'd yet. Treat frame IDs, byte order,
and the analog protocol mode as assumptions to confirm before trusting a real
reading, the same way `mapMax` above was a documented case of trusting an unverified
mapping.

- **TX Base ID**: `0x700` — the unit's datasheet default. Doesn't collide with the
  Syvecs frames (`0x600`-`0x614`), but this car's MCE18 unit hasn't been confirmed
  to actually be configured at its default.
- **Frame `0x700`** (Base ID): bytes 0-1 = AIN0, assigned to the fuel sender.
  AIN1-3 (bytes 2-7) are unused. AIN0 was picked over AIN1/AIN2/AIN6 specifically
  because those three double as Frequency 2/3/4 inputs per the datasheet — using
  AIN0 keeps the frequency-capable analogs free for future use.
- **Frame `0x702`** (Base ID+2): byte 2 = bit-masked DIN0-7. Bit *N* = DIN *N*
  (the datasheet doesn't spell out bit order — assumed). DIN5 is unassigned:
  Cruise (`cruiseControl`) comes from the Syvecs stream instead (`0x601` slot 1,
  cruiseState — see the frame map above), not this expander. DIN6 is
  deliberately left unassigned too: the datasheet reuses that same pin for its
  Frequency 1 input (`**TX Base ID+3`, "Frequency 1 - DIN6"), so it's kept free
  rather than double-booked. DIN7 is also unassigned, reserved for a possible
  future use — Auto/Manual (`transmissionAuto`) comes from the Syvecs stream
  instead (`0x605` slot 3, ManualAuto_U12 — see the frame map above), not this
  expander.

  | Bit | Signal |
  |---|---|
  | DIN0 | `leftIndicator` |
  | DIN1 | `rightIndicator` |
  | DIN2 | `axleLift` |
  | DIN3 | `lowBeams` |
  | DIN4 | `highBeams` |
  | DIN5 | *(unassigned — cruise sourced from Syvecs instead)* |
  | DIN6 | *(unassigned — reserved for Frequency 1)* |
  | DIN7 | *(unassigned)* |

- **Analog scaling**: AIN0 can be configured on the unit as either raw 0-1023 ADC
  counts or pre-scaled 0-5000mV (the datasheet's own table shows both units without
  saying which is the power-on default). `CanBus::decodeFrame()` assumes raw
  0-1023 counts spanning the AIN's own 0-5000mV full scale (`kMce18AinRawMax` /
  `kMce18AinFullScaleMv` in `canbus.cpp`). The fuel sender itself is 0V empty / 4V
  full (linear, per sender spec) — narrower than the AIN's 0-5V range — so the
  converted mV is then scaled again against `kFuelSenderFullScaleMv` (4000mV) to
  get 0..1. Empty/full direction (low counts = empty) is assumed.

Needs, before trusting this on the road: `candump can0` with the MCE18 powered to
confirm its actual configured base ID and analog protocol mode, and a known fuel
level (e.g. empty vs. full tank) to calibrate the AIN0 scaling.

### Debugging

- `candump can0` (can-utils is included in the image) to watch raw traffic.
- `ip -details link show can0` to check bitrate/link state.
- `CanBus` logs `[canbus] ...` lines to stderr on connect/bind failures and reconnects — check `journalctl -u ultima-app` (BeaglePlay logs to journald, see `ultima-app.service`).
