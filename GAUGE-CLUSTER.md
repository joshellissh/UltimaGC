# Ultima Gauge Cluster — Qt App Structure & CAN Bus Integration

Board-agnostic reference for the Qt5/QML app itself (`ultima-app/`)
and the live CAN data it displays. This content applies regardless of which board is
running the dash. For board-specific build/boot/flash instructions, see
`beagley-ai/NOTES.md`.

## Qt App Structure

### Source Files

All in `ultima-app/`. Don't copy these listings verbatim
elsewhere — read `ultima-app.pro` / `qml.qrc` directly for the current file set; they
change as the app evolves and a stale copy is worse than no copy.

- **`ultima-app.pro`** — qmake project file: `HEADERS`/`SOURCES` has grown well
  beyond the original `odostore`/`canbus`/`systemclock` trio to include the
  camera + 360 surround-view + calibration stack (`camerafeed`, `cameraview`,
  `cameracalibration`, `warpmesh`, `shadermanager`, `surroundtexture`,
  `surroundview`, `calibrationstore`, `dmabuftexture`); `RESOURCES += qml.qrc`. A `ultima_dev_sim`
  CONFIG flag (set by `scripts/dev-build-wsl.sh`) defines `ULTIMA_SIMULATE` to
  force simulated gauge data on a Linux dev build too — macOS dev builds
  simulate unconditionally regardless of this flag (see `canbus.h`/`.cpp`
  below).
- **`main.cpp`** — creates `OdoStore` for persistent odometer, `CanBus` as the live
  gauge data source, `SystemClock` for the time-set screen, and `SystemStats` for
  board stats that aren't CAN signals; exposes all four to QML as context properties
  (`odoStore`, `sim`, `systemClock`, `sysStats`), plus a `bootTime` timestamp used
  for startup-latency logging (`fprintf(stderr, ...)` at each init stage, read from
  `/proc/uptime`). Saves odometer state on SIGTERM/SIGINT via `CanBus::save()`
  (falls back to `OdoStore::save()` directly if `CanBus` isn't up yet).
- **`odostore.h` / `odostore.cpp`** — `OdoStore` is a `QObject` with `totalOdo` and
  `tripOdo` properties. Reads `/data/odometer.json` on construction (defaults to
  0.0 / 0.0 if missing — was 2347.0 / 0.0 until the odometer was reset
  2026-08-19, see `beagley-ai/NOTES.md`). `save()` slot writes JSON.
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
  `scripts/dev-build.sh`). Read-only — `CanBus` has no Tx path.
- **`systemclock.h` / `systemclock.cpp`** — lets the QML time-set screen
  (`SetTimeScreen.qml`) push a new wall-clock time to the kernel via
  `clock_settime()`, with a best-effort write-through to a battery-backed hardware
  RTC at `/dev/rtc0` if one is present. No-op
  on non-Linux dev builds. Also exposes `timeIsValid()`, which the dash clock
  (`main.qml`) uses to show `--:--` instead of a stale boot-default time during the
  brief post-boot window before the RTC's real time lands — see "Dash clock
  doesn't persist a manual set" under [Startup & Clock Behavior](#startup--clock-behavior) below.
- **`systemstats.h` / `systemstats.cpp`** — polls board stats that aren't CAN
  signals, currently just the SoC die temperature, for the Diagnostics screen's
  `source: "sysStats"` channel. On Linux, scans `/sys/class/thermal/thermal_zone*/type`
  for a CPU/MPU/MAIN-ish zone (falls back to `thermal_zone0`, logged either way —
  this repo has no local J722S devicetree to confirm zone numbering against) and
  polls its `temp` file every 2s. Simulates a slow sine drift around 72 °C on
  non-Linux dev builds and Linux `ultima_dev_sim` builds, same convention as
  `CanBus`'s simulator.
### QML Files

The app consists of 11 QML files, all in `ultima-app/qml/`:

- **`main.qml`** — Root layout: 4 gauge needles over the Bavarian background layers, boost gauge (trapezoid black overlay with PSI readout), turn signal indicators, top indicator row (oil, check engine, beams, battery, coolant — warn icons flash at 300ms), a separate low-fuel badge (`icon_fuel_low.png`, turns the fuel-pump icon red under 1/4 tank — see `sim.lowFuelWarn`), gear indicator (Bahnschrift SemiBold font, P/R/N/1-7), odometer + trip odometer with reset button, touch feedback dot. Tapping the car (either lights state) opens `Camera360Screen` — this used to be a dedicated `icon_360.png` tap target; that icon and the binding were removed (2026-08-18) in favor of tapping the car itself. While a turn signal is active (and hazards aren't), `leftCamOverlay`/`rightCamOverlay` pop up a bordered `CameraView` fed by `cameraFeed3`/`cameraFeed4` respectively, reprojected through `mirror.frag`'s virtual-mirror view (see `CameraView`'s `mirrorViewSide` property below) — a live blind-spot mirror replacement, gone the instant the signal (a static level, not a blink waveform — see the CAN section) drops or hazards latch on. On startup, a ~1s splash-to-cluster intro (`introFrac`/`introTransitionDone`) shows `splash_screen.png` with `splash_car_start.png` overlaid at the exact offset it occupies in that art (262,167,1104x364 — pixel-matched against the source PNGs, not eyeballed), then fades the splash background out while the car cutout shrinks/moves onto `car_lights_off`'s alpha-bbox rect (627,479,346x128) and fades away, handing off to the real car artwork underneath. This exists so Qt's first frame is pixel-identical to what the pre-Qt `ultima-splash` framebuffer splash already painted (see `beagley-ai/NOTES.md`), keeping the modeset handoff invisible. Only once that finishes does the ~2s self-test (`startupActive`/`startupFrac`/`startupFlash`) sweep every needle to max and back and flash every icon before handing off to live `sim.*` values — real gauge/warning bindings are `startupActive ? <test value> : sim.<prop>`. Includes a periodic (30s) save timer for odometer persistence via `sim.save()`. All gauge values bind to the `sim` context property, which is the `CanBus` C++ object, not this file's `SimEngine.qml`. Hosts `SetTimeScreen`, `DiagnosticScreen`, `CameraGridScreen`, `Camera360Screen`, and `RearCameraScreen` as overlays; a swipe left/right on the main dash opens `DiagnosticScreen`/`CameraGridScreen` respectively — `PageIndicator` (below) shows dots for that 3-screen swipe layout.
- **`CircularGauge.qml`** — Reusable needle gauge component: rotates `needle.png` over a transparent item positioned at the gauge center. Configurable start/end angles, counter-clockwise mode, needle size/pivot, optional debug arc overlay
- **`SetTimeScreen.qml`** — Time-set overlay that calls into `SystemClock` (the `systemClock` context property) to push a new wall-clock time
- **`DiagnosticScreen.qml`** — Full-screen grid of every CAN2 signal this build actually decodes today (ECU + MCE18 — see below), showing the raw decoded value/enum behind each dash gauge or icon rather than just its needle position or lit/unlit state, plus a handful of non-CAN board stats (currently just CPU temp, from the `sysStats` context property — see `systemstats.h`). Opened by swiping left anywhere on the main dash, closed by swiping right
- **`Camera360Screen.qml`** — Full-screen camera overlay, toggled open/closed (250ms cross-fade via a `Behavior on opacity`) by tapping the car (`car_lights_off`/`car_lights_on`) on the main dash to open, tapping anywhere on the overlay to close — reverse gear no longer auto-opens this screen (see `RearCameraScreen.qml` below). This used to be a dedicated `icon_360.png` tap target; that icon was removed (2026-08-18) in favor of tapping the car itself. Feeds the 4 raw streams from the mycam004m driver (`cameraFeed1`..`cameraFeed4` context properties set up in `main.cpp`, each a `CameraFeed` opening one of the driver's stable `/dev/mycam/cam1`..`cam4` symlinks — fake or real backend, see `~/code/mycam004m/docs/ultima-app-integration.md`) into `SurroundView` (`surroundview.h`), which stitches them into one top-down birds-eye composite via a precomputed per-camera fisheye/ground-plane warp mesh + feather blend. `car_360.png` (full 1600x720, car pre-centered on transparent background) is drawn on top over the vehicle-mask hole SurroundView's mesh leaves unpainted — not unused art, this is what makes the composite read as a car surrounded by ground rather than a car-shaped hole. Calibration (per-camera position/yaw/pitch/FOV, vehicle dimensions, ground extent, wedge overlap) defaults to a placeholder rig (`cameracalibration.cpp`'s `defaultCalibration()`) but is live-tunable and persisted via the gear-icon settings panel (`CalibrationSettingsScreen.qml`) — seams/ground-plane scale are only exactly right once real measured camera-mount data replaces the placeholder. If every feed has failed or none produce a frame within 2s of opening, `showPlaceholder` just hides `SurroundView` and the car icon, leaving the plain black backdrop — the older `simulated_cameras.png` fallback art was removed (2026-08-18), there's no dedicated placeholder image anymore.
- **`RearCameraScreen.qml`** — Full-screen single-camera overlay, opened/closed automatically on reverse gear (`main.qml`'s `reverseGear`), like a real backup camera. Shows `cameraFeed2` (the rear feed — see `Camera360Screen.qml`'s `[front, rear, left, right]` feeds-order comment) raw via `CameraView`, no stitching, no calibration UI. Same 250ms opacity cross-fade and 2s live-frame timeout-to-blank pattern as `Camera360Screen`, scoped to just the one feed.
- **`CameraGridScreen.qml`** — Persistent swipeable screen (mirror of `DiagnosticScreen`, reached by swiping right off the main dash) showing the same 4 mycam004m feeds as a plain 2x2 grid, one quadrant per physical camera, no stitching — a raw-feed debug view alongside `Camera360Screen`'s stitched one. Separate file/screen rather than a mode of `Camera360Screen` so that screen's opacity-fade behavior stays untouched.
- **`CalibrationSettingsScreen.qml`** — Slide-in panel (opened from `Camera360Screen`'s gear icon) for live-tuning `SurroundView`'s stitching parameters; every edit writes to `CalibrationStore` (`calibrationstore.h`, persisted to `/data/calibration.json`, same load/save pattern as `OdoStore`) and re-warps the mesh immediately via `calibrationChanged()`, no restart needed. Fixed lens/render properties (image size, principal point, fisheye k1-k4, mesh grid resolution) aren't exposed here.
- **`CalibrationParamRow.qml`** — Reusable row (label, live value, -/+ steppers with press-and-hold repeat) used throughout `CalibrationSettingsScreen`; touch-friendly for the small numeric steps calibration tuning needs.
- **`PageIndicator.qml`** — Small reusable bottom dot row (active dot widens, LINE-design-system style) shown on the three swipeable screens (`CameraGridScreen`, `main.qml`, `DiagnosticScreen`) so the dots always match the physical left/right swipe layout. Informational only, not a tap target.
- **`SimEngine.qml`** — **Not loaded at runtime.** Bundled only as a reference/potential `--demo` fallback; it predates `CanBus` and was the original simulated data source before real CAN integration. Speed wanders through city/suburban/highway/stop phases, RPM derived from gear ratios, automatic gear selection (P/R/N/1-7), fuel consumption, coolant temp, boost pressure (0-30 PSI). `CanBus::simulateTick()` reimplements this same phase logic directly in C++ so it can drive the live `sim` property — see [CAN Bus Integration](#can-bus-integration-syvecs-s7)

### Asset Files

Images are in `ultima-app/assets/images/`, fonts in
`ultima-app/assets/fonts/`. `main.qml`'s background uses the "Bavarian" theme (source PSDs/exports in
`Ultima Gauge Cluster/Bavarian/` outside this repo): 4 stacked full-canvas
(1600x720) layers instead of one flat image — back to front:

| File | Purpose |
|------|---------|
| `boost_circle.png` | Backmost layer — boost gauge dial (opaque, own black backing) |
| `background_overlay.png` | Gauge/dial face overlay (tick marks, TOTAL/TRIP text area, mini fuel/coolant arcs; alpha) |
| `car_lights_off.png` | Centered car render (alpha) |
| `car_lights_on.png` | Same, headlights lit — `visible: sim.lowBeams \|\| sim.highBeams`, loaded `asynchronous: true` so the off-state boot path isn't blocked decoding it |
| `splash_screen.png` | Full 1600x720 "ULTIMA RS" splash art — a copy of repo-root `splash screen.png` (which also feeds the pre-Qt `ultima-splash` framebuffer splash; keep both in sync when the art changes). Shown full-bleed for the startup intro, then faded out — see `main.qml`'s intro section above |
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

## Startup & Clock Behavior

### Boot splash (pre-Qt framebuffer splash)

Before the Qt app's first frame, a tiny userspace program paints a static splash
straight to the framebuffer, so the panel shows the ULTIMA art within ~2s of
power-on instead of staying black until Qt comes up (~5s later). It's a separate
component from the Qt app — `ultima-splash`, a board-layer recipe — but the app is
written to hand off from it invisibly, so the mechanism is documented here alongside
the app-side intro it pairs with.

Why userspace and not the kernel's built-in fbcon boot logo (`CONFIG_LOGO`): on the
DRM-fbdev-emulation display stack this project runs, the kernel's `fb_show_logo()`
never actually fires — verified by dumping `/dev/fb0` after a clean boot (zero
non-zero bytes until a console write forces one). The fbcon→fb0 pixel path itself
works; only the dedicated boot-logo blit is dead. So the splash is a ~90-line C
program (`ultima-splash.c`) that `mmap`s `/dev/fb0` and blits a raw image, rather
than fighting that path.

Key design choices, all load-bearing:

- **Plain `/dev/fb0`, never `/dev/dri/card0`.** The program never opens the DRM
  device and never takes DRM master, so it can't contend with the Qt app's
  `eglfs_kms` display. The fbdev→eglfs handoff happens on every boot regardless of
  the splash; the splash only changes which pixels sit in the buffer when that
  handoff occurs, it doesn't add one. (With this stack's legacy KMS the app's first
  `drmModeSetCrtc` does force one real modeset — but Qt issues it only when it's
  already about to render, so the single blank frame lands in a sub-200ms window
  immediately followed by real content, not as its own visible flash.)
- **Ships as a headerless raw pixel blob, not a PNG.** Decoding PNG on-target would
  mean adding libpng/zlib to a deliberately dependency-light read-only rootfs, for
  one static build-time-known image. The art is converted once, host-side, to the
  exact in-memory byte order the panel wants (`PIL:
  Image.convert("RGB").tobytes("raw", "BGRX")` — on a little-endian target, bytes
  `[B,G,R,pad]` read back as native `0x00RRGGBB` / XRGB8888), so the on-target code
  is a plain stride-aware `read()` loop with zero pixel-format conversion.
- **Refuses to guess.** It draws only if the blob's byte count matches `xres*yres*4`
  for whatever panel is actually attached; a mismatch aborts rather than
  scaling/cropping — the same "refuse to guess" stance as its bpp check.
- **Runs as a `oneshot` systemd unit** ordered before the app
  (`DefaultDependencies=no`, `WantedBy=sysinit.target`, `Before=ultima-app.service`
  — ordering only, not a dependency). It draws once and exits; the image persists
  because the display holds the last-committed buffer, not because a process keeps
  running — so there's no long-running process or DRM master to synchronize the
  app's start against.

App-side handoff: `main.qml`'s startup intro (`introFrac`/`introTransitionDone`) is
authored so Qt's very first frame is pixel-identical to what `ultima-splash` already
painted (same art — the `splash_screen.png` app asset is a copy of the same source
art the framebuffer splash uses; keep them in sync), then animates from there into
the live cluster. That's what makes the framebuffer→Qt modeset handoff read as one
continuous splash rather than a black blink. `scripts/dev-build.sh --boot` replays
this whole timeline on the desktop dev build (opt-in via `ULTIMA_SPLASH_IMAGE`) so
the boot flow can be screen-recorded without hardware.

### Dash clock doesn't persist a manual set

Two app-side behaviors around the wall clock, both worth knowing when touching
`SystemClock` (`systemclock.h`/`.cpp`) or the dash clock in `main.qml`:

**A manual time-set can be silently overwritten by NTP on a networked bench.**
`SystemClock::setTime()` calls `clock_settime()` directly (plus a best-effort
write-through to a hardware RTC at `/dev/rtc0`) — it does *not* go through
`org.freedesktop.timedate1`. So if an NTP daemon like `systemd-timesyncd` is
running, it never learns a manual override happened and re-syncs the clock out from
under it, the way `timedatectl set-time` would have prevented. This only bites on
the bench, where the board has wired Ethernet; the deployed car has no network, so
NTP was never a legitimate clock source there anyway. The shipped image disables
`systemd-timesyncd` for exactly this reason (`ultima_mask_timesyncd` in the image
recipe), so on a real car the RTC is the only thing that sets the clock.

**The dash shows `--:--` until the clock is known-good, to hide the boot-default
blip.** At power-on the system clock holds a stale boot-default until an
RTC-to-system-clock load runs (`hwclock --hctosys` or equivalent), so for a brief
window the app is up but the time is wrong. Rather than flash that wrong time, the
clock binding in `main.qml` shows `--:--` until `SystemClock::timeIsValid()` flips
true, on the same per-second `Timer` tick that already re-reads the time.

`timeIsValid()` reads `/dev/rtc0` directly (`RTC_RD_TIME`) and compares it against
`time(nullptr)`; it **fails closed** — reports invalid if the RTC device isn't
present yet or the read fails — which correctly covers both a board with no
battery-backed RTC at all and the brief post-boot window before the RTC driver has
registered `/dev/rtc0`.

That RTC comparison replaced a first attempt that compared the wall clock against
the binary's own build timestamp (`__DATE__`/`__TIME__`), on the theory that the
stale boot-default is always earlier than build time. It regressed to a permanent
`--:--`: the build container's clock had drifted hours *ahead* of real time (an
OrbStack VM clock-drift case), so the embedded "build time" was itself in the future
and the real clock could never catch up to it. The lesson generalizes past the
drift — **no build machine's clock is trustworthy ground truth**, so a sanity check
should compare against a value already proven live on the *target* (here, the same
RTC that's supposed to set the clock in the first place), never against anything
baked in at build time.

## CAN Bus Integration (Syvecs S7+)

The dash gets live engine/vehicle data from the car's Syvecs S7+ ECU over CAN, decoded by `CanBus` (`canbus.h`/`canbus.cpp`) and exposed to QML as the `sim` context property.

### Hardware

- **Adapter**: MikroE CAN SPI 3.3V click (MCP2515 CAN controller + SN65HVD230 transceiver), on the board's SPI bus. Screw terminals on the click board wire to the ECU's **B2 (CAN_H) / B3 (CAN_L)** pins. Replaced an earlier ODrive USB-CAN adapter (`gs_usb`/candleLight) — retired in favor of this SPI click so the dash doesn't depend on a USB dongle.
- **Termination**: the click has a termination jumper (J2) — populate it since this board sits at one end of the Syvecs S7+'s CAN2 bus, which has **no on-board termination** of its own; without it (or an external 120 Ω resistor across CAN_H/CAN_L at the ECU end) the bus won't terminate correctly.
- **Device tree**: the CAN controller is a device-tree-instantiated SPI device — the MCP2515 is declared on the board's SPI bus (chip-select 0, its own 10 MHz crystal as a `fixed-clock`, INT on a GPIO line) by a kernel-recipe DT patch. Falcon boot bakes the dtb in at kernel build time — there's no U-Boot-proper stage here to apply a runtime `.dtbo` overlay, so this has to be a compile-time change, not a cape-manager-style overlay.
- **Kernel support**: `CONFIG_CAN`, `CAN_RAW`, `SPI_OMAP24XX`, `CAN_MCP251X` are enabled, all built in (`=y`, not `=m`) via a kernel config fragment — this is a device-tree-instantiated SPI device, not a hotplugged USB one, and DT-platform-device module coldplug has bitten this project before, so building the CAN driver in avoids relying on it.
- **Bring-up**: a udev rule matches the mcp251x-driven SPI network device and runs `ip link set can0 type can bitrate 1000000 && ip link set up can0` automatically as soon as the driver registers the interface (`beagley-ai/meta-ultima-beagley-ai-src/recipes-ultima/ultima-app/files/70-can.rules`). `CanBus` doesn't assume `can0` exists at startup regardless — it retries `socket(PF_CAN)` + `bind()` every 1s until the interface appears, so app start never has to race udev.

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

### MCE18 CAN Bus Expander (DSS-Configured 2026-08-21, Wiring Still Unconfirmed)

Fills part of the gap above: `flvlA` (fuel level) and six booleans that have no
Syvecs channel at all (`leftIndicator`, `rightIndicator`, `hazard`, `axleLift`,
`lowBeams`, `highBeams`) are read from a CANchecked-protocol MCE18 CAN bus expander
instead of the ECU. Source: `docs/MCE18-manual_V3+V4.pdf` (CANchecked MCE18 manual,
Rev 2.0).
`transmissionAuto` and `cruiseControl` are deliberately *not* part of this — both
come from the Syvecs stream instead (`0x605` slot 3, ManualAuto_U12, and `0x601`
slot 1, cruiseState — see the frame map above).

**The unit's own configuration (via DSS) was set 2026-08-21**: protocol
`CANchecked 0-5000mV`, TX Base ID `0x700` (datasheet default, kept), CAN speed
1 Mbit/s (matching Syvecs CAN2 and `can0`'s udev-configured bitrate — DSS
defaults to 500 kbps, which would have silently produced zero traffic). That
resolves the frame ID and analog-units questions this section used to flag as
open. Still unconfirmed: DIN bit order and AIN0's empty/full direction — no
candump against a known switch state or known fuel level has been done yet.
DIN0/DIN1/DIN7's static-level-vs-flasher-waveform question (see below) was
confirmed 2026-08-18, ahead of the rest of this frame.

- **TX Base ID**: `0x700` — the unit's datasheet default, confirmed set in DSS
  2026-08-21. Doesn't collide with the Syvecs frames (`0x600`-`0x614`).
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

- **Analog scaling**: AIN0-8 are configured (protocol `CANchecked 0-5000mV`) to
  arrive as millivolts directly — the unit does its own ADC-to-voltage conversion
  internally, so `CanBus::decodeFrame()` reads AIN0 as mV with no raw-counts
  scaling step. The fuel sender itself is 1V empty / 4V full (linear, per sender
  spec) — narrower than the AIN's 0-5V range — so fuel level is
  `(mv − kFuelSenderEmptyMv) / (kFuelSenderFullScaleMv − kFuelSenderEmptyMv)`
  (1000mV/4000mV in `canbus.cpp`), not a plain ratio against full scale. Empty/full
  direction (low mV = empty) is still assumed, not candump-confirmed.

- **AUX1-3 outputs (not currently driven by `ultima-app` — `CanBus` is
  Rx-only, no Tx path)**: bench-tested 2026-08-21 by hand with `cansend` on
  the RX Base ID (`0x640`, default, confirmed working as-is — byte 0 = AUX1,
  byte 1 = AUX2, byte 2 = AUX3, `0`=OFF/`1`=ON). Confirmed bit *N* = AUX
  *N+1* echoed back in `0x702` byte 3 (same bit-order convention as the DIN
  mask above). **Important for whenever something does drive these: the
  MCE18's outputs are watchdog-based, not sticky.** A single one-shot
  `cansend` doesn't hold — the unit reverts to OFF well under a second after
  the command frame stops arriving (confirmed by repeating the RX frame
  every 20ms with a shell loop; a single send showed no effect at all in a
  2s window). Whatever eventually commands these (Syvecs Tx config, a future
  `CanBus` Tx path, etc.) needs to send the AUX state periodically, not once.

This bench test used the ODrive USB-CAN adapter (`gs_usb`, temporarily
re-added alongside the MCP2515 SPI click — see `70-can.rules` — since the
click isn't physically seated on the SPI header yet) and confirmed real MCE18
traffic end-to-end: `0x700`/`0x701`/
`0x702` all present, byte 7 of `0x702` (`version`) read `0x74` (116),
consistent with a real unit on firmware past the internal-mapper cutoff
(104) mentioned in the manual.

Needs, before trusting this on the road: `candump can0` with the MCE18 wired
to real car switches to confirm DIN bit order (toggle one switch at a time
and watch which bit moves) and a known fuel level (e.g. empty vs. full tank)
to confirm the AIN0 empty/full direction and calibration.

### Debugging

- `candump can0` (can-utils is included in the image) to watch raw traffic.
- `ip -details link show can0` to check bitrate/link state.
- `CanBus` logs `[canbus] ...` lines to stderr on connect/bind failures and reconnects — check `journalctl -u ultima-app` (the app logs to journald, see `ultima-app.service`).
- `main.cpp` polls for a handful of trigger files under `/tmp` every frame or so (dev/debug only, works over SSH on real hardware too, not just the dev-sim build): `/tmp/ultima-screenshot.request` (optionally containing an output path, default `/tmp/ultima-screenshot.png`) grabs a frame; `/tmp/ultima-camtest.request` containing `open`/`close`/`360open`/... drives the camera overlays without touching CAN; `/tmp/ultima-indicator.request` containing `left`/`right`/`hazard` toggles turn signals the same way the debug L/R/H keys do. Each trigger file is deleted after being read.
- `CameraFeed` supports `ULTIMA_CAM_IMAGE_DIR` (env var) to serve real static photos instead of synthetic test bars from the fake mycam004m backend — useful for eyeballing `CameraView`'s mirror-view reprojection (`mirror.frag`) against real-world content instead of a test pattern.
- Camera performance env vars (all real-hardware; see "Camera framerate: root cause found" below for the 2026-08-26 investigation that added them): `ULTIMA_CAM_FPS_LOG=1` prints per-feed arrived/published/decoded rates + convert times and per-`CameraView` render stats every ~2s; `ULTIMA_CAM_ZEROCOPY=0` disables the default zero-copy dma-buf display path (`dmabuftexture.h`) and forces the converted-QImage path everywhere; `ULTIMA_CAM_FANOUT=1` points `cameraFeed2..4` at `cameraFeed1`'s object so every camera surface renders the one attached camera — a 4-camera load proxy for a single-camera bench.

### Camera frame paths (zero-copy vs converted)

Two parallel paths carry frames out of `CameraFeed` (full rationale + measured numbers in "Camera framerate: root cause found" below): the **zero-copy path** (default on target) exports each V4L2 capture buffer as a dma-buf and renderers (`CameraView` — grid tiles, rear screen, mirror overlays) import it as a `GL_TEXTURE_EXTERNAL_OES` via `DmaBufTextureSet`, the GPU sampling UYVY directly — no CPU convert, no upload; and the **converted path** (`SurroundView`'s stitch, plus the whole macOS/sim build) where a per-feed capture thread NEON-decodes UYVY→RGBA8888 at half resolution into `currentFrame()` QImages, but only while a consumer is registered (`CameraFeed::addFrameConsumer()`, driven by `SurroundView`'s visibility). `ShaderManager::ExternalSampler` lets `blit.frag`/`mirror.frag` serve both paths from one source file. The capture buffers themselves are requested CPU-cacheable (`V4L2_MEMORY_FLAG_NON_COHERENT`) — reading the default uncached mapping is what made the original 2026-08-25 "3fps grid" bug.

### Camera framerate: root cause found (4 fps → 25 fps)

This is the investigation behind the two-path design above. On the first real
cameras the 4-up grid ran at ~4 fps, and the obvious fixes (decode at lower
resolution, back off failed-feed retries) barely moved it — the real cost was
somewhere else entirely.

**Root cause of the ~4 fps: uncached V4L2 buffer reads.** Per-stage timers split one
frame into DQBUF-drain / UYVY→RGBA convert / GPU upload / render — the convert alone
was **~240 ms/frame**, everything else 0.05–5 ms. The `videobuf2-dma-contig` MMAP
capture buffers are mapped **uncached** (dma-coherent, Normal-NC), and the scalar
converter did ~1M single-byte loads per frame from that mapping — every load a full
DRAM round-trip (~240 ns). The GUI thread sat ~240 ms inside each convert, so it only
serviced the capture socket-notifier ~4×/s; the driver was delivering 25 fps all
along, the surplus buffers just got dropped for lack of a free one. ("Not CPU-bound,
72% idle" was a busybox `top` misread — one core was pegged in stalled loads, the
other three idle.)

The fix is two independent halves:

1. **Ask V4L2 for CPU-cacheable buffers** — `VIDIOC_REQBUFS` with
   `V4L2_MEMORY_FLAG_NON_COHERENT` (kernel ≥5.15). Needs no driver change *if* the
   capture driver advertises the capability (`V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS`,
   and the flag echoes back set); vb2 cache-invalidates on DQBUF, and the CPU only
   ever reads the buffers, so there's no dirty-line hazard.
2. **NEON UYVY→RGBA convert** (`vld4q_u8`, one 64-byte de-interleaving load per 16
   pixels) instead of byte-at-a-time scalar.

Cached buffers are by far the bigger lever — measured convert cost, same 960×540
frame:

| variant | convert ms/frame | decoded fps |
|---|---|---|
| scalar, uncached (the original shipping code) | 240 | 4.1 |
| NEON, uncached | 64 | 15 |
| scalar, **cached** | 8.9 | 25 |
| NEON, **cached** | **3.6** | **25** |

(That's the basis for the "never byte-scan an uncached DMA buffer" rule — wide loads
help, but *cached* is the real fix.)

**Second bottleneck (4-camera scaling): the render thread.** With one camera fixed
at 25 fps, a 4-camera load proxy (`ULTIMA_CAM_FANOUT=1`) saturated the render thread
at ~9.5 fps/quadrant, 104% of a core: 4× `glTexSubImage2D` of ~2 MB RGBA each plus 4
FBO passes. Nothing upload-shaped reached 25. The fix that ends the category is the
**zero-copy path**: export each V4L2 buffer as a dma-buf (`VIDIOC_EXPBUF`), import it
as an EGLImage (`eglCreateImageKHR` with `EGL_LINUX_DMA_BUF_EXT`, `DRM_FORMAT_UYVY`,
BT.601 limited-range hints) bound to a `GL_TEXTURE_EXTERNAL_OES`, and let the GPU
sampler do UYVY→RGB — no CPU convert, no upload at all. The PowerVR stack here
supports it (`EGL_EXT_image_dma_buf_import` + `GL_OES_EGL_image_external_essl3` + a
UYVY pipe format). Measured, 4 quadrants:

| path | per-quadrant fps | render thread |
|---|---|---|
| convert + `glTexSubImage2D` | 9.5 | 104% (saturated) |
| **zero-copy external texture** | **25.0** | **22%** |

What shipped (see the code comments for the load-bearing detail): capture+convert
moved off the GUI thread onto a per-feed capture thread; cached buffers; NEON; a
zero-copy buffer-lending mailbox with refcounts (`camerafeed.{h,cpp}`) — retired
buffers re-`QBUF`'d on the capture thread. Converted QImages still exist but only
while a consumer registers (`addFrameConsumer`, driven by `SurroundView`'s
visibility). `dmabuftexture.{h,cpp}` does the EGLImage import (libEGL via `dlopen`,
so the macOS/sim build has no link dependency), with session tracking so a stream
restart drops stale imports — EGLImages pin CMA (≈6 buffers × 4 MB × 4 cams = 96 MB
of a 128 MB pool), so a hidden view that misses the drop window self-heals on the
1 s reconnect. `cameraview.cpp` renders the external texture when available and keeps
the QImage path as the fallback; `ShaderManager::ExternalSampler` rewrites
`sampler2D`→`samplerExternalOES` at shader load so `blit.frag`/`mirror.frag` serve
both paths from one source. One color note: the GPU samples BT.601 **limited-range**
(matching what the cameras actually emit), where the old CPU path assumed full-range
— zero-copy frames look slightly higher-contrast, which is the *more correct*
rendering, not a regression.

**A hidden trap worth its own note: `QQuickFramebufferObject::update()` renders even
when the item is invisible.** First full-app fanout runs came in at 17 fps, not the
prototype's 25 — per-renderer logging showed **seven** `CameraView`s rendering, not
four: the two hidden mirror overlays and the hidden rear-camera screen were
re-rendering every frame, and the mirror views' per-fragment fisheye reprojection is
GPU-heavy. `update()` schedules the FBO pass regardless of item visibility, so a
`frameReady → update()` connection on a hidden item is a full wasted pass. Two fixes,
both needed: the `frameReady`/`streamingChanged` handlers now check `isVisible()`
before calling `update()`; and `CameraGridScreen.qml` — which "closes" by sliding to
`x: -parent.width` while staying `visible: true` — now binds `visible: isOpen`, since
the `isVisible()` guard alone still let all four off-screen tiles render whenever a
feed streamed.
