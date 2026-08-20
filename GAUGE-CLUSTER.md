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

- **`ultima-app.pro`** — qmake project file: `HEADERS`/`SOURCES` has grown well
  beyond the original `odostore`/`canbus`/`systemclock` trio to include the
  camera + 360 surround-view + calibration stack (`camerafeed`, `cameraview`,
  `cameracalibration`, `warpmesh`, `shadermanager`, `surroundtexture`,
  `surroundview`, `calibrationstore`); `RESOURCES += qml.qrc`. A `ultima_dev_sim`
  CONFIG flag (set by `scripts/dev-build-wsl.sh`) defines `ULTIMA_SIMULATE` to
  force simulated gauge data on a Linux dev build too — macOS dev builds
  simulate unconditionally regardless of this flag (see `canbus.h`/`.cpp`
  below).
- **`main.cpp`** — creates `OdoStore` for persistent odometer, `CanBus` as the live
  gauge data source, `SystemClock` for the time-set screen, and `BluetoothManager`
  for the Bluetooth pairing screen; exposes all four to
  QML as context properties (`odoStore`, `sim`, `systemClock`, `bluetooth`), plus a `bootTime`
  timestamp used for startup-latency logging (`fprintf(stderr, ...)` at each
  init stage, read from `/proc/uptime`). Saves odometer state on SIGTERM/SIGINT via
  `CanBus::save()` (falls back to `OdoStore::save()` directly if `CanBus` isn't up
  yet).
- **`odostore.h` / `odostore.cpp`** — `OdoStore` is a `QObject` with `totalOdo` and
  `tripOdo` properties. Reads `/data/odometer.json` on construction (defaults to
  0.0 / 0.0 if missing — was 2347.0 / 0.0 until the odometer was reset
  2026-08-19, see `beagleplay-falcon/NOTES.md`). `save()` slot writes JSON.
  `main.qml`'s periodic save
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
  `scripts/dev-build.sh`). Also the only Tx path in the class: `setAux()`/
  `allAuxOff()` command the MCE18's AUX outputs (see the MCE18 section below
  and `ANDROID-BLE-INTEGRATION.md`) — everything else `CanBus` does is
  read-only.
- **`systemclock.h` / `systemclock.cpp`** — lets the QML time-set screen
  (`SetTimeScreen.qml`) push a new wall-clock time to the kernel via
  `clock_settime()`, with a best-effort write-through to a battery-backed hardware
  RTC at `/dev/rtc0` if one is present (BeaglePlay has one, onboard BQ32002). No-op
  on non-Linux dev builds. Also exposes `timeIsValid()`, which the dash clock
  (`main.qml`) uses to show `--:--` instead of a stale boot-default time during the
  brief post-boot window before the RTC's real time lands — see
  `beagleplay-falcon/NOTES.md` "Dash clock doesn't persist a manual set".
- **`bluetoothmanager.h` / `bluetoothmanager.cpp`** — `BluetoothManager` backs the
  Bluetooth pairing screen (`BluetoothScreen.qml`), exposed to QML as the `bluetooth`
  context property. The dash acts as a BLE *peripheral* — it advertises itself as
  `"Ultima RS"` (`QLowEnergyController::createPeripheral()` + `startAdvertising()`),
  not a central that scans for nearby devices (a phone doesn't advertise itself as a
  connectable BLE peripheral, so a scan-based design wouldn't find it) — see
  `beagleplay-falcon/NOTES.md` "Bluetooth via CC1352P7" for the full reasoning.
  **Neither iOS nor Android's built-in Bluetooth settings actually pairs with a bare
  peripheral like this** (tested against real hardware and a real Android phone,
  2026-08-19 — an earlier assumption that Android's Settings would "just work" turned
  out to be wrong) — reaching this from a phone needs a dedicated app; see
  `ANDROID-BLE-INTEGRATION.md` for the GATT service/characteristics an Android
  companion app would use — the Ultima AUX Control Service (4 AUX outputs on
  the MCE18, via `CanBus::setAux()`) is implemented as of 2026-08-20, but not
  yet hardware-verified: `QLowEnergyController::addService()` was previously
  unproven on this stack (only advertising + connection were), and this
  session had no board to test against — see that doc's own repeated caveat
  and its "Open questions for whoever implements the dash side" list before
  trusting this beyond compiling. `QBluetoothLocalDevice`
  is still used for pairing confirmation (adapter-level, not role-specific).
  Advertising is started once at boot (Linux target only, from `main.cpp`) and left
  running — it does not track whether `BluetoothScreen` is open, so a companion app
  can connect at any time, not only while someone is at the touchscreen. On the
  macOS dev build this stays fully lazy instead (`main.cpp` guards the boot-time
  `startAdvertising()` call to `#if defined(__linux__)`): merely constructing
  `QBluetoothLocalDevice`/`QLowEnergyController` is enough to trigger a CoreBluetooth
  permission prompt there, which isn't worth imposing on every local QML-iteration run.
  `#if QT_VERSION < QT_VERSION_CHECK(6,0,0)` gates the pairing-confirmation/PIN-display
  API (`pairingDisplayConfirmation`, `pairingDisplayPinCode`, `pairingConfirmation()`),
  which Qt6 removed entirely — only reachable on the Qt5/BlueZ target build anyway.
  BeaglePlay's Bluetooth controller (the onboard CC1352P7, not the WL1807) has no
  BR/EDR radio at all.

### QML Files

The app consists of 12 QML files, all in `ultima-app/src/`:

- **`main.qml`** — Root layout: 4 gauge needles over the Bavarian background layers, boost gauge (trapezoid black overlay with PSI readout), turn signal indicators, top indicator row (oil, check engine, beams, battery, coolant — warn icons flash at 300ms), a separate low-fuel badge (`icon_fuel_low.png`, turns the fuel-pump icon red under 1/4 tank — see `sim.lowFuelWarn`), gear indicator (Bahnschrift SemiBold font, P/R/N/1-7), odometer + trip odometer with reset button, touch feedback dot, a small Canvas-drawn Bluetooth glyph tucked in the bottom-right corner (opens `BluetoothScreen`, see below). Tapping the car (either lights state) opens `Camera360Screen` — this used to be a dedicated `icon_360.png` tap target; that icon and the binding were removed (2026-08-18) in favor of tapping the car itself. While a turn signal is active (and hazards aren't), `leftCamOverlay`/`rightCamOverlay` pop up a bordered `CameraView` fed by `cameraFeed3`/`cameraFeed4` respectively, reprojected through `mirror.frag`'s virtual-mirror view (see `CameraView`'s `mirrorViewSide` property below) — a live blind-spot mirror replacement, gone the instant the signal (a static level, not a blink waveform — see the CAN section) drops or hazards latch on. On startup, a ~1s splash-to-cluster intro (`introFrac`/`introTransitionDone`) shows `splash_screen.png` with `splash_car_start.png` overlaid at the exact offset it occupies in that art (262,167,1104x364 — pixel-matched against the source PNGs, not eyeballed), then fades the splash background out while the car cutout shrinks/moves onto `car_lights_off`'s alpha-bbox rect (627,479,346x128) and fades away, handing off to the real car artwork underneath. This exists so Qt's first frame is pixel-identical to what `beagleplay-falcon`'s pre-Qt `ultima-splash` framebuffer splash already painted (see that project's NOTES.md), keeping the modeset handoff invisible. Only once that finishes does the ~2s self-test (`startupActive`/`startupFrac`/`startupFlash`) sweep every needle to max and back and flash every icon before handing off to live `sim.*` values — real gauge/warning bindings are `startupActive ? <test value> : sim.<prop>`. Includes a periodic (30s) save timer for odometer persistence via `sim.save()`. All gauge values bind to the `sim` context property, which is the `CanBus` C++ object, not this file's `SimEngine.qml`. Hosts `SetTimeScreen`, `DiagnosticScreen`, `CameraGridScreen`, `Camera360Screen`, and `RearCameraScreen` as overlays; a swipe left/right on the main dash opens `DiagnosticScreen`/`CameraGridScreen` respectively — `PageIndicator` (below) shows dots for that 3-screen swipe layout.
- **`CircularGauge.qml`** — Reusable needle gauge component: rotates `needle.png` over a transparent item positioned at the gauge center. Configurable start/end angles, counter-clockwise mode, needle size/pivot, optional debug arc overlay
- **`SetTimeScreen.qml`** — Time-set overlay that calls into `SystemClock` (the `systemClock` context property) to push a new wall-clock time
- **`BluetoothScreen.qml`** — Bluetooth pairing overlay, opened by the bottom-right glyph on `main.qml`. Advertising itself is always on (started once at boot, see the `bluetoothmanager.h`/`.cpp` bullet above) — this screen doesn't start or stop it, only shows "Discoverable as ..." status, the currently-connected device (if any) or a "Waiting for a connection..." state, and a pairing-confirmation panel (code display + accept/reject, or just a wait state for "Just Works"/PIN-display pairing) when `bluetooth.pendingPairAddress` is set. No scan/device list — a phone connects *to* the dash, not the other way around, but not via the phone's own Bluetooth settings (see the `bluetoothmanager.h`/`.cpp` bullet above — that path doesn't work on current iOS or Android). Same overlay styling as `SetTimeScreen` (full-screen black, bahnschrift/range fonts, `DashButton`-style controls)
- **`DiagnosticScreen.qml`** — Full-screen grid of every CAN2 signal this build actually decodes today (ECU + MCE18 — see below), showing the raw decoded value/enum behind each dash gauge or icon rather than just its needle position or lit/unlit state. Opened by swiping left anywhere on the main dash, closed by swiping right
- **`Camera360Screen.qml`** — Full-screen camera overlay, toggled open/closed (250ms cross-fade via a `Behavior on opacity`) by tapping the car (`car_lights_off`/`car_lights_on`) on the main dash to open, tapping anywhere on the overlay to close — reverse gear no longer auto-opens this screen (see `RearCameraScreen.qml` below). This used to be a dedicated `icon_360.png` tap target; that icon was removed (2026-08-18) in favor of tapping the car itself. Feeds the 4 raw streams from the mycam004m driver (`cameraFeed1`..`cameraFeed4` context properties set up in `main.cpp`, each a `CameraFeed` opening one of the driver's stable `/dev/mycam/cam1`..`cam4` symlinks — fake or real backend, see `~/code/mycam004m/docs/ultima-app-integration.md`) into `SurroundView` (`surroundview.h`), which stitches them into one top-down birds-eye composite via a precomputed per-camera fisheye/ground-plane warp mesh + feather blend. `car_360.png` (full 1600x720, car pre-centered on transparent background) is drawn on top over the vehicle-mask hole SurroundView's mesh leaves unpainted — not unused art, this is what makes the composite read as a car surrounded by ground rather than a car-shaped hole. Calibration (per-camera position/yaw/pitch/FOV, vehicle dimensions, ground extent, wedge overlap) defaults to a placeholder rig (`cameracalibration.cpp`'s `defaultCalibration()`) but is live-tunable and persisted via the gear-icon settings panel (`CalibrationSettingsScreen.qml`) — seams/ground-plane scale are only exactly right once real measured camera-mount data replaces the placeholder. If every feed has failed or none produce a frame within 2s of opening, `showPlaceholder` just hides `SurroundView` and the car icon, leaving the plain black backdrop — the older `simulated_cameras.png` fallback art was removed (2026-08-18), there's no dedicated placeholder image anymore.
- **`RearCameraScreen.qml`** — Full-screen single-camera overlay, opened/closed automatically on reverse gear (`main.qml`'s `reverseGear`), like a real backup camera. Shows `cameraFeed2` (the rear feed — see `Camera360Screen.qml`'s `[front, rear, left, right]` feeds-order comment) raw via `CameraView`, no stitching, no calibration UI. Same 250ms opacity cross-fade and 2s live-frame timeout-to-blank pattern as `Camera360Screen`, scoped to just the one feed.
- **`CameraGridScreen.qml`** — Persistent swipeable screen (mirror of `DiagnosticScreen`, reached by swiping right off the main dash) showing the same 4 mycam004m feeds as a plain 2x2 grid, one quadrant per physical camera, no stitching — a raw-feed debug view alongside `Camera360Screen`'s stitched one. Separate file/screen rather than a mode of `Camera360Screen` so that screen's opacity-fade behavior stays untouched.
- **`CalibrationSettingsScreen.qml`** — Slide-in panel (opened from `Camera360Screen`'s gear icon) for live-tuning `SurroundView`'s stitching parameters; every edit writes to `CalibrationStore` (`calibrationstore.h`, persisted to `/data/calibration.json`, same load/save pattern as `OdoStore`) and re-warps the mesh immediately via `calibrationChanged()`, no restart needed. Fixed lens/render properties (image size, principal point, fisheye k1-k4, mesh grid resolution) aren't exposed here.
- **`CalibrationParamRow.qml`** — Reusable row (label, live value, -/+ steppers with press-and-hold repeat) used throughout `CalibrationSettingsScreen`; touch-friendly for the small numeric steps calibration tuning needs.
- **`PageIndicator.qml`** — Small reusable bottom dot row (active dot widens, LINE-design-system style) shown on the three swipeable screens (`CameraGridScreen`, `main.qml`, `DiagnosticScreen`) so the dots always match the physical left/right swipe layout. Informational only, not a tap target.
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
| `splash_screen.png` | Full 1600x720 "ULTIMA RS" splash art — a copy of repo-root `splash screen.png` (which also feeds `beagleplay-falcon`'s pre-Qt framebuffer splash; keep both in sync when the art changes). Shown full-bleed for the startup intro, then faded out — see `main.qml`'s intro section above |
| `splash_car_start.png` | 1104x364 cutout of the splash art's car, tightly cropped to its alpha bbox — pixel-identical to the region at (262,167) in `splash_screen.png`. Overlaid there at launch, then animated onto `car_lights_off`'s rect as part of the startup intro |
| `needle.png` | Gauge needle image (rotated by CircularGauge) |
| `left_indicator.png` | Turn signal arrow icon (mirrored for right) |
| `range.regular.ttf` | Font for odometer and boost PSI display |
| `bahnschrift._semibold.ttf` | Bahnschrift SemiBold font for gear indicator |
| `icon_oil_pressure.png`, `icon_check_engine.png`, `icon_battery.png`, `icon_coolant_warn.png`, `icon_low_beam.png`, `icon_high_beam.png`, `icon_axle_lift.png`, `icon_cruise.png` | ISO 7000-style dashboard warning icons (white on transparent) |
| `icon_fuel_low.png` | Red fuel-pump badge shown in place of the normal fuel icon once `sim.lowFuelWarn` (fuel < 1/4 tank) |
| `car_360.png` | `Camera360Screen`'s car-icon overlay — full 1600x720, car pre-centered on transparent background, drawn over the vehicle-mask hole `SurroundView`'s warp mesh leaves unpainted |

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
| `0x60E` | 2-3 | gear | Syvecs enum 0=Unknown 1=R 2=N 3..10=1st..8th (this car has 7 forward gears; 10/8th falls back to Neutral) → QML index into "PRN1234567": 0=P 1=R 2=N 3..9=1st..7th |
| `0x60E` | 4-5 | vbat | V × 0.001 (unsigned); `batteryWarn` if v < 12.5 |
| `0x60F` | 0-1 | vehicleSpeed | raw × 0.0223694 → mph; drives odometer accumulation |

**Not on CAN2 in the current SCal config:**
- `flvlA` (fuel level) — no Syvecs channel for this; now sourced from the MCE18 expander instead (see below), not SCal.
- `sensorWarningLevel` — `checkEngine` currently derives from `limpMode` alone.
- `mapMax` (boost, `0x614`/frame 21) — previously documented here as verified and wired to the boost gauge, but the xlsx built from `CAN2.png` (the source of truth for this mapping) shows frame 21 as all SPARE. Decode removed from `CanBus::decodeFrame()`. The boost gauge isn't stuck at 0 waiting on this, though — it was rewired to `map1A` (`0x600` bytes 6-7, see the frame map above) instead, which *is* SCal-confirmed.

**Not on CAN at all:** `driveMode` (no Syvecs channel exists, and no physical selector is known to exist in the car) is a real READ+NOTIFY property, but on real hardware it's only ever set to its default (`"SPORT"`) — nothing decodes it from a CAN frame or an MCE18 input. It's a 3-state QString, which doesn't fit a single digital input the way the MCE18-sourced booleans below do; wiring it (e.g. to a real drive-mode selector switch) is unstarted. The dev-build simulator (`simulateTick()`) is the only thing that ever changes it, for layout review.

### MCE18 CAN Bus Expander (Unverified — Datasheet Default, Not Wire-Confirmed)

Fills part of the gap above: `flvlA` (fuel level) and six booleans that have no
Syvecs channel at all (`leftIndicator`, `rightIndicator`, `hazard`, `axleLift`,
`lowBeams`, `highBeams`) are read from a CANchecked-protocol MCE18 CAN bus expander
instead of the ECU. Source: "CAN2 MCE18 Mapping.pdf" (CANchecked MCE18 manual, Rev
2.0).
`transmissionAuto` and `cruiseControl` are deliberately *not* part of this — both
come from the Syvecs stream instead (`0x605` slot 3, ManualAuto_U12, and `0x601`
slot 1, cruiseState — see the frame map above).

**Most of this section is a datasheet default, not verified against this car**
— no MCE18 unit has been on the bench or candump'd yet. Treat frame IDs, byte order,
and the analog protocol mode as assumptions to confirm before trusting a real
reading, the same way `mapMax` above was a documented case of trusting an unverified
mapping. The one exception: DIN0/DIN1/DIN7's static-level-vs-flasher-waveform
question (see below) was confirmed 2026-08-18, ahead of the rest of this frame.

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
  rather than double-booked. DIN7 is `hazard`, treated the same as DIN0/DIN1
  (`leftIndicator`/`rightIndicator`): **confirmed (2026-08-18) a static
  "switch pressed" level** — on while the signal/hazard is engaged, off the
  instant it's cancelled — not the flasher's own on/off waveform this doc
  previously assumed. The dash's visible blink is synthesized in QML from a
  shared clock instead (see `main.qml`'s `blinkTimer`), not read off the CAN
  bit directly; camera overlays and the hazard latch bind straight to the raw
  level with no hold timer. Auto/Manual (`transmissionAuto`) comes from the
  Syvecs stream instead (`0x605` slot 3, ManualAuto_U12 — see the frame map
  above), not this expander.

  | Bit | Signal |
  |---|---|
  | DIN0 | `leftIndicator` |
  | DIN1 | `rightIndicator` |
  | DIN2 | `axleLift` |
  | DIN3 | `lowBeams` |
  | DIN4 | `highBeams` |
  | DIN5 | *(unassigned — cruise sourced from Syvecs instead)* |
  | DIN6 | *(unassigned — reserved for Frequency 1)* |
  | DIN7 | `hazard` |

- **Analog scaling**: AIN0 can be configured on the unit as either raw 0-1023 ADC
  counts or pre-scaled 0-5000mV (the datasheet's own table shows both units without
  saying which is the power-on default). `CanBus::decodeFrame()` assumes raw
  0-1023 counts spanning the AIN's own 0-5000mV full scale (`kMce18AinRawMax` /
  `kMce18AinFullScaleMv` in `canbus.cpp`). The fuel sender itself is 1V empty / 4V
  full (linear, per sender spec) — narrower than the AIN's 0-5V range — so fuel
  level is `(mv − kFuelSenderEmptyMv) / (kFuelSenderFullScaleMv − kFuelSenderEmptyMv)`
  (1000mV/4000mV in `canbus.cpp`), not a plain ratio against full scale. Empty/full
  direction (low counts = empty) is assumed.

Needs, before trusting this on the road: `candump can0` with the MCE18 powered to
confirm its actual configured base ID and analog protocol mode, and a known fuel
level (e.g. empty vs. full tank) to calibrate the AIN0 scaling.

**AUX outputs (Tx, the opposite direction from everything above)**: this car's
MCE18 is a V4 unit — 4 low-side AUX outputs, 3A each, AUX4 PWM-capable — driven
by `CanBus::setAux()`/`allAuxOff()` over a separate command frame (datasheet
default receive ID `0x640`, distinct from the `0x700` read-side base ID above
and equally unconfirmed against this car). This is the CAN backing for the
Ultima AUX Control Service; see `ANDROID-BLE-INTEGRATION.md` for the full
frame layout, the BLE GATT contract, and the failsafe design — not
duplicated here since that doc is the source of truth for it.

### Debugging

- `candump can0` (can-utils is included in the image) to watch raw traffic.
- `ip -details link show can0` to check bitrate/link state.
- `CanBus` logs `[canbus] ...` lines to stderr on connect/bind failures and reconnects — check `journalctl -u ultima-app` (BeaglePlay logs to journald, see `ultima-app.service`).
- `main.cpp` polls for a handful of trigger files under `/tmp` every frame or so (dev/debug only, works over SSH on real hardware too, not just the dev-sim build): `/tmp/ultima-screenshot.request` (optionally containing an output path, default `/tmp/ultima-screenshot.png`) grabs a frame; `/tmp/ultima-camtest.request` containing `open`/`close`/`360open`/... drives the camera overlays without touching CAN; `/tmp/ultima-indicator.request` containing `left`/`right`/`hazard` toggles turn signals the same way the debug L/R/H keys do. Each trigger file is deleted after being read.
- `CameraFeed` supports `ULTIMA_CAM_IMAGE_DIR` (env var) to serve real static photos instead of synthetic test bars from the fake mycam004m backend — useful for eyeballing `CameraView`'s mirror-view reprojection (`mirror.frag`) against real-world content instead of a test pattern.
