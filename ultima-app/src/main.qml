import QtQuick 2.15
import QtQuick.Window 2.15
import Ultima 1.0

Window {
    id: root
    width: 1600
    height: 720
    visibility: Window.Windowed
    color: "black"

    // Startup self-test: on launch, sweep every needle to full deflection
    // and back while flashing every tell-tale icon, like a normal car's
    // cluster does on power-up. Each needle/icon binding below checks
    // startupActive and falls back to its real sim-driven expression once
    // the sequence finishes, so this is purely a launch-time overlay — it
    // doesn't change steady-state behavior.
    property bool startupActive: true
    property real startupFrac: 0
    property bool startupFlash: false

    // Boot splash simulation — dev-build only, opt-in via ULTIMA_SPLASH_IMAGE
    // (set by `scripts/dev-build.sh --boot`), empty/unset otherwise. Mimics
    // the on-target timeline from beagleplay-falcon/NOTES.md's "Boot splash
    // implemented" section: ~2.1s black, then the splash art for ~5.1s,
    // then a hard cut — real hardware's handoff is a single abrupt modeset,
    // not a fade — straight into the self-test sweep below. Gating that
    // sweep on splashDone (instead of starting it immediately) matters: on
    // hardware the sweep's SequentialAnimation only exists once this QML is
    // constructed, which is exactly when the splash-to-cluster handoff
    // happens, so on real boot the sweep always starts right as the splash
    // disappears. Without this gate the dev-build sweep would run its own
    // 2.2s and finish while still hidden behind the overlay.
    property bool splashDone: splashImagePath === ""

    // Debug-only: opens/closes CameraGridScreen/Camera360Screen without
    // touch input, for on-device testing over SSH where no touchscreen/
    // input-injection tool is available — see main.cpp's matching
    // file-trigger QTimer, same pattern as that file's screenshot-request
    // trigger.
    function debugSetCameraGrid(open) {
        if (open) cameraGridScreen.open()
        else cameraGridScreen.close()
    }
    function debugSetCamera360(open) {
        if (open) camera360Screen.open()
        else camera360Screen.close()
    }

    Timer {
        running: !splashDone
        interval: 2100
        onTriggered: bootSplashImage.visible = true
    }
    Timer {
        running: !splashDone
        interval: 2100 + 5150
        onTriggered: { bootOverlay.visible = false; splashDone = true }
    }

    SequentialAnimation {
        running: splashDone
        PropertyAction { target: root; property: "startupFlash"; value: true }
        NumberAnimation { target: root; property: "startupFrac"; to: 1; duration: 1000; easing.type: Easing.OutQuad }
        PauseAnimation { duration: 200 }
        NumberAnimation { target: root; property: "startupFrac"; to: 0; duration: 1000; easing.type: Easing.InQuad }
        PropertyAction { target: root; property: "startupFlash"; value: false }
        PropertyAction { target: root; property: "startupActive"; value: false }
    }

    // FPS counter state — driven by real render-thread frame swaps
    // (QQuickWindow::frameSwapped), not a fixed-interval guess, so it
    // reflects actual redraw rate under this build's software rendering
    // backend (see ultima-app.pro / NOTES.md). frameSwapped fires on the
    // scene graph render thread; QML auto-marshals the connection back to
    // this (GUI-thread) property write the same way any cross-thread Qt
    // signal/slot does, so no explicit locking is needed here.
    property int _fpsFrameCount: 0
    onFrameSwapped: _fpsFrameCount++

    // Background layers, back to front: boost circle, gauge overlay, car
    //
    // The boost ring wraps the tachometer (same center/sweep as rpmGauge:
    // 1251,343 / 270deg-503deg CW). It's fully covered by a pie wedge at
    // zero boost, radially exposing more of the ring as boost builds —
    // sweeping in the same direction as the tach's own needle travel.
    Canvas {
        id: boostRing
        // Was anchors.fill: parent (full 1600x720) — under software
        // rendering that's a full-screen CPU backing store + paint just to
        // draw a wedge around the tach. Bounded to a box around the gauge
        // instead (same center as rpmGauge, with margin since the ring
        // graphic itself may extend past the 600x600 gauge face).
        //
        // 640 is deliberate, not a round-number guess: the tach center
        // (1251,343) sits only 343px from the window's top edge and 349px
        // from its right edge (window is 1600x720), so the largest
        // symmetric box centered there that stays inside the window is
        // 698x686 — anything bigger clips negative on x/y and made the
        // 9-arg drawImage below throw "index size error" on real hardware
        // (Qt5/software backend; the macOS Qt6 dev build didn't catch this,
        // it apparently clamps out-of-bounds source rects instead of
        // throwing — don't trust that build alone for this kind of check
        // again). 640 leaves real margin inside that limit.
        x: 1251 - width / 2
        y: 343 - height / 2
        width: 640
        height: 640

        readonly property real centerX: width / 2
        readonly property real centerY: height / 2
        readonly property real startAngle: 270
        readonly property real endAngle: 503
        readonly property real maxBoost: 30

        property real displayBoost: startupActive ? startupFrac * maxBoost : sim.boost
        Behavior on displayBoost {
            NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
        }

        Component.onCompleted: loadImage("qrc:/boost_circle.png")
        onImageLoaded: requestPaint()
        onDisplayBoostChanged: requestPaint()

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            if (!isImageLoaded("qrc:/boost_circle.png"))
                return

            function toRad(deg) {
                return (deg - 90) * Math.PI / 180
            }

            var frac = Math.max(0, Math.min(1, displayBoost / maxBoost))
            var sweepEnd = startAngle + frac * (endAngle - startAngle)
            var r = Math.max(width, height) * 1.5

            ctx.save()
            ctx.beginPath()
            ctx.moveTo(centerX, centerY)
            ctx.arc(centerX, centerY, r, toRad(startAngle), toRad(sweepEnd), false)
            ctx.closePath()
            ctx.clip()
            // boost_circle.png is a full 1600x720 asset with the ring drawn
            // at its natural (x, y) position in that composition — since
            // this canvas is no longer full-screen, crop the matching
            // window-space region out of the source image (9-arg drawImage)
            // rather than scaling the whole image into the smaller canvas,
            // which would squash/misplace the ring.
            ctx.drawImage("qrc:/boost_circle.png", x, y, width, height, 0, 0, width, height)
            ctx.restore()
        }
    }
    Image {
        anchors.fill: parent
        source: "qrc:/background_overlay.png"
    }
    // Was anchors.fill: parent (full 1600x720) — the car artwork itself only
    // occupies a small box in the middle of that canvas (the rest is fully
    // transparent margin), so the GPU was compositing a full-screen quad to
    // show a couple hundred pixels of real content every frame. Cropped to
    // the asset's actual alpha bounding box and positioned at the matching
    // (x, y) offset instead — pixel-identical result, much smaller quad.
    // Same fix already applied to boostRing above for the same reason.
    Image {
        x: 627; y: 409
        width: 346; height: 128
        source: "qrc:/car_lights_off.png"
        visible: !sim.lowBeams && !sim.highBeams
    }
    Image {
        x: 569; y: 409
        width: 404; height: 165
        source: "qrc:/car_lights_on.png"
        visible: sim.lowBeams || sim.highBeams
        // CanBus's lowBeams/highBeams both default false (canbus.h), so
        // "off" is the true boot state — decoding this full-frame image
        // synchronously on every boot is wasted work on the critical path
        // to first frame. Async keeps instant on/off toggling once loaded;
        // it just means the very first boot with lights already on could
        // show a blank layer for a frame or two until the decode finishes.
        asynchronous: true
    }

    // Periodic odometer save (every 30s) — CanBus owns integration and pushes
    // the latest values into OdoStore.
    Timer {
        interval: 30000
        running: true
        repeat: true
        onTriggered: sim.save()
    }

    // Cruise control indicator — upper-left of the speedometer's black
    // interior, painted above the background art but under the needle
    // (declared before speedGauge, so it stacks behind it).
    Image {
        x: 273 - width / 2; y: 264 - height / 2
        width: 50; height: 50
        fillMode: Image.PreserveAspectFit
        source: "qrc:/icon_cruise.png"
        visible: startupActive ? startupFlash : sim.cruiseControl
    }

    // Left gauge: Speedometer — pivot at (351, 342)
    CircularGauge {
        id: speedGauge
        x: 351 - width / 2
        y: 342 - height / 2
        width: 600
        height: 600
        value: startupActive ? minValue + startupFrac * (maxValue - minValue) : sim.speed
        minValue: 0
        maxValue: 220
        startAngle: 216.5
        endAngle: 450.5
    }

    // Right gauge: Tachometer — pivot at (1251, 343)
    CircularGauge {
        id: rpmGauge
        x: 1251 - width / 2
        y: 343 - height / 2
        width: 600
        height: 600
        value: startupActive ? minValue + startupFrac * (maxValue - minValue) : sim.rpm / 1000
        minValue: 0
        maxValue: 8
        startAngle: 270
        endAngle: 503
    }

    // Bottom-left: Fuel level — pivot at (149, 602)
    CircularGauge {
        id: fuelGauge
        x: 149 - width / 2
        y: 602 - height / 2
        width: 200
        height: 200
        value: startupActive ? minValue + startupFrac * (maxValue - minValue) : sim.fuelLevel
        minValue: 0
        maxValue: 1
        startAngle: 217
        endAngle: 307.5
        needleWidth: 28
        needleHeight: 100
        pivotX: 14
        pivotY: 74
    }

    // Bottom-right: Coolant temp — pivot at (1453, 602)
    CircularGauge {
        id: coolantGauge
        x: 1453 - width / 2
        y: 602 - height / 2
        width: 200
        height: 200
        value: startupActive ? minValue + startupFrac * (maxValue - minValue) : sim.coolantTemp
        minValue: 160
        maxValue: 240
        startAngle: 142
        endAngle: 53.5
        counterClockwise: true
        needleWidth: 28
        needleHeight: 100
        pivotX: 14
        pivotY: 74
    }

    // Left turn signal indicator — while hazards are engaged (hazardLatched,
    // defined below), visibility tracks sim.hazard exclusively rather than
    // OR'ing it with sim.leftIndicator. leftIndicator/hazard are independent
    // raw waveforms with no shared phase (independent debug blink timers in
    // CanBus, and no guarantee of phase alignment from real CAN either), so
    // if a turn signal was already flashing on its own cycle when hazards
    // started, OR'ing the two together made left/right flash out of sync
    // with each other instead of together. hazardLatched (rather than raw
    // sim.hazard) is what makes this an override instead of a fallback that
    // still races leftIndicator's phase during hazard's off-phase — it stays
    // true across hazard's full on+off cycle, matching main.qml's
    // overlay-latch hold-time comment below. Camera overlays are separately
    // suppressed during hazards by the same hazardLatched (see that
    // property's comment) — on some wirings hazards also drive
    // leftIndicator/rightIndicator themselves, which would otherwise pop
    // the cameras too.
    Image {
        x: 25
        y: 23
        source: "qrc:/left_indicator.png"
        visible: startupActive ? startupFlash : (hazardLatched ? sim.hazard : sim.leftIndicator)
    }

    // Right turn signal indicator (mirrored) — see left indicator above for
    // why hazardLatched overrides rather than ORs with rightIndicator.
    Image {
        x: 1600 - 25 - width
        y: 23
        source: "qrc:/left_indicator.png"
        visible: startupActive ? startupFlash : (hazardLatched ? sim.hazard : sim.rightIndicator)
        mirror: true
    }

    // Debug-only keyboard trigger for the turn signals/hazards — functional
    // on every build, real hardware included if a keyboard happens to be
    // plugged in (see canbus.h's Q_INVOKABLE comment). L/R toggle
    // sim.leftIndicator/rightIndicator via CanBus's
    // debugToggleLeftIndicator()/debugToggleRightIndicator(); H toggles
    // sim.hazard via debugToggleHazard(). Window itself can't host
    // Keys.onPressed (that attached property is Item-only, Window isn't an
    // Item), hence this focused child Item covering the whole dash.
    Item {
        anchors.fill: parent
        focus: true

        // Debounce: confirmed on real hardware (2026-08-18, journalctl
        // trace) that a single physical keypress can generate two distinct
        // Keys.onPressed events, both isAutoRepeat=false, arriving under
        // 350ms apart (no blink tick logged between them) — not a Qt/QML
        // dispatch bug (only one Keys.onPressed handler exists anywhere in
        // this app), but the input layer itself double-delivering. Likely
        // explanation: this board's DELL keyboard enumerates as three
        // separate USB HID interfaces (base Keyboard, System Control,
        // Consumer Control — see /proc/bus/input/devices), and Qt's
        // evdevkeyboard plugin appears to pick the same physical key up
        // from more than one of those device nodes. Since
        // debugToggle*Indicator() is a plain flip, an on+off pair
        // milliseconds apart nets to "closes right after opening" — this
        // guard drops a same-key repress within 250ms of the last accepted
        // one, well above the sub-350ms gap measured, well below anything
        // a human would intend as two separate presses of a toggle key.
        property double lastLeftPressMs: 0
        property double lastRightPressMs: 0
        property double lastHazardPressMs: 0
        Keys.onPressed: (event) => {
            if (event.isAutoRepeat) return
            var now = Date.now()
            if (event.key === Qt.Key_L) {
                if (now - lastLeftPressMs < 250) return
                lastLeftPressMs = now
                sim.debugToggleLeftIndicator()
                event.accepted = true
            } else if (event.key === Qt.Key_R) {
                if (now - lastRightPressMs < 250) return
                lastRightPressMs = now
                sim.debugToggleRightIndicator()
                event.accepted = true
            } else if (event.key === Qt.Key_H) {
                if (now - lastHazardPressMs < 250) return
                lastHazardPressMs = now
                sim.debugToggleHazard()
                event.accepted = true
            }
        }
    }

    // Turn-signal camera overlays — blind-spot-style popups showing cam3/
    // cam4 (see surroundview.h's class comment for the cam-to-side mapping
    // caveat), horizontally centered on the speedometer/tachometer needle
    // pivot while the matching turn signal is on, but vertically centered on
    // the screen rather than the pivot. A gauge's needle pivot is exactly
    // its own (x + width/2, y + height/2) regardless of needleWidth/pivotX/Y
    // — see CircularGauge.qml — so the x's reuse speedGauge/rpmGauge's own
    // documented pivot x-coordinates (351) / (1251) rather than re-deriving
    // them.
    //
    // sim.leftIndicator/rightIndicator are the raw blinking lamp state, not
    // a latched "signal engaged" flag — the turn-signal Image elements above
    // read them directly with no _warnFlash-style timer of their own, unlike
    // the oil/battery/coolant warn icons, which only makes sense if the CAN
    // input itself is already the on/off flasher waveform. Gating the
    // overlay directly on that would toggle cameraFeed3/4.active in sync
    // with the flasher and the feed would never outlive the "off" half of
    // the cycle long enough to produce a frame (camerafeed.h: first frame
    // is tens-to-hundreds of ms after activation) — it'd sit on "NO SIGNAL"
    // forever. Latched instead: each rising edge (off->on transition) (re)
    // arms a hold, so it stays continuously active across the flasher's
    // off-phase and only drops a beat after the signal actually stops
    // blinking.
    //
    // The hold must outlast a full flasher PERIOD, not just its off-phase:
    // sim.leftIndicator toggles on a 350ms timer (see canbus.cpp), so
    // rising edges land exactly 700ms apart (one on-phase + one off-phase
    // per cycle), not every 350ms. An earlier version used a 700ms hold —
    // exactly equal to that 700ms rising-edge spacing, a zero-margin race
    // where the hold's own expiry and its next retrigger land at the same
    // instant, so which one QML processes first is luck; on real hardware
    // this reliably lost often enough to look like "closes immediately
    // after opening." 1000ms gives ~300ms of slack over the 700ms period.
    // Standard automotive flasher rate is assumed at 60-120/min (700ms
    // matches the low end) — not measured against this car's actual rate,
    // revisit if it flickers during real turn signals or lingers too long
    // after they cancel.
    property bool leftIndicatorLatched: false
    property bool rightIndicatorLatched: false
    // Hazard latch — same hold-across-a-full-flasher-period pattern as the
    // two above, armed on sim.hazard rising edges. Two jobs:
    // 1. Turn-signal Images above use it to make hazards an override rather
    //    than an OR: leftIndicator/hazard are independent waveforms with no
    //    shared phase, so OR'ing them let left/right flash out of sync with
    //    each other whenever a turn signal was already engaged when hazards
    //    started. Gating on hazardLatched (not raw sim.hazard) means the
    //    override holds across hazard's off-phase too, not just its
    //    on-phase — a raw sim.hazard ternary would still let
    //    leftIndicator's independent phase leak through during every
    //    hazard off-phase.
    // 2. Gates both camera overlays below regardless of what
    //    leftIndicator/rightIndicator are doing: on wiring where the hazard
    //    switch flashes the same bulb circuits DIN0/DIN1 read (see
    //    canbus.h's hazard Q_PROPERTY comment for why that's plausible,
    //    unverified either way), leftIndicatorLatched/rightIndicatorLatched
    //    would otherwise also arm from the hazard-driven blinking and pop
    //    the cameras — which is exactly the "no cameras" hazards asked for.
    // Latched rather than a raw !sim.hazard/sim.hazard check for both jobs,
    // for the same reason as the other two latches: m_hazard itself blinks,
    // so an instantaneous check would flip every off-phase.
    property bool hazardLatched: false
    readonly property bool leftCamOverlayActive: !startupActive && leftIndicatorLatched && !hazardLatched
    readonly property bool rightCamOverlayActive: !startupActive && rightIndicatorLatched && !hazardLatched

    Timer { id: leftIndicatorHold; interval: 1000; onTriggered: leftIndicatorLatched = false }
    Timer { id: rightIndicatorHold; interval: 1000; onTriggered: rightIndicatorLatched = false }
    Timer { id: hazardHold; interval: 1000; onTriggered: hazardLatched = false }
    Connections {
        target: sim
        function onLeftIndicatorChanged() {
            if (sim.leftIndicator) { leftIndicatorLatched = true; leftIndicatorHold.restart() }
        }
        function onRightIndicatorChanged() {
            if (sim.rightIndicator) { rightIndicatorLatched = true; rightIndicatorHold.restart() }
        }
        function onHazardChanged() {
            if (sim.hazard) { hazardLatched = true; hazardHold.restart() }
        }
    }

    // Single owner of every CameraFeed's active state, so it's never fought
    // over by multiple imperative writers. CameraGridScreen/Camera360Screen
    // used to toggle feeds[i].active themselves in their own open()/close()
    // — but a plain JS assignment to a property permanently breaks whatever
    // binding was previously driving it, so a Binding element here trying to
    // do the same job would get silently and permanently disconnected the
    // first time either screen closed. Centralizing it as declarative
    // Bindings here, with those screens' open()/close() only driving their
    // own slide/fade state, avoids that: every consumer's "I want this feed
    // on" is OR'd together at one single point of truth instead.
    Binding { target: cameraFeed1; property: "active"; value: cameraGridScreen.isOpen || camera360Screen.visible }
    Binding { target: cameraFeed2; property: "active"; value: cameraGridScreen.isOpen || camera360Screen.visible }
    Binding { target: cameraFeed3; property: "active"; value: cameraGridScreen.isOpen || camera360Screen.visible || leftCamOverlayActive }
    Binding { target: cameraFeed4; property: "active"; value: cameraGridScreen.isOpen || camera360Screen.visible || rightCamOverlayActive }

    Item {
        id: leftCamOverlay
        z: 50
        height: 400
        // Container itself is aspect-correct at the new height (rather than
        // a fixed-width box cropping an internally-aspect-correct
        // CameraView, which was just a heavily zoomed-in vertical sliver of
        // the real image) — width tracks CameraFeed's negotiated aspect
        // ratio, falling back to 16:9 before the first frame negotiates a
        // real size. x's binding on width keeps this horizontally centered
        // on the pivot regardless of which aspect ratio it resolves to; y
        // centers on the screen instead of the pivot (see comment above).
        width: cameraFeed3.frameHeight > 0
               ? Math.round(height * (cameraFeed3.frameWidth / cameraFeed3.frameHeight))
               : Math.round(height * 16 / 9)
        x: 351 - width / 2
        anchors.verticalCenter: parent.verticalCenter
        visible: leftCamOverlayActive

        Rectangle { anchors.fill: parent; color: "black"; border.color: "white"; border.width: 3 }

        CameraView {
            feed: cameraFeed3
            anchors.fill: parent
            visible: !cameraFeed3.failed
        }

        Text {
            anchors.centerIn: parent
            color: "white"
            font.family: bahnschriftFont.name
            font.pixelSize: 16
            text: cameraFeed3.failed ? "CAM FAILED" : "NO SIGNAL"
            visible: !cameraFeed3.streaming
        }
    }

    Item {
        id: rightCamOverlay
        z: 50
        height: 400
        // See leftCamOverlay above for why width is derived here rather
        // than fixed.
        width: cameraFeed4.frameHeight > 0
               ? Math.round(height * (cameraFeed4.frameWidth / cameraFeed4.frameHeight))
               : Math.round(height * 16 / 9)
        x: 1251 - width / 2
        anchors.verticalCenter: parent.verticalCenter
        visible: rightCamOverlayActive

        Rectangle { anchors.fill: parent; color: "black"; border.color: "white"; border.width: 3 }

        CameraView {
            feed: cameraFeed4
            anchors.fill: parent
            visible: !cameraFeed4.failed
        }

        Text {
            anchors.centerIn: parent
            color: "white"
            font.family: bahnschriftFont.name
            font.pixelSize: 16
            text: cameraFeed4.failed ? "CAM FAILED" : "NO SIGNAL"
            visible: !cameraFeed4.streaming
        }
    }

    // Warning flash timer (300ms cycle)
    property bool _warnFlash: true
    Timer {
        interval: 300
        running: sim.oilPressureWarn || sim.batteryWarn || sim.coolantWarn
        repeat: true
        onTriggered: _warnFlash = !_warnFlash
        onRunningChanged: if (running) _warnFlash = true
    }

    // Axle lift indicator — centered below the gear readout at (800, 558)
    Image {
        x: 800 - width / 2; y: 558 - height / 2
        source: "qrc:/icon_axle_lift.png"
        visible: startupActive ? startupFlash : sim.axleLift
    }

    // 360 view icon — centered at (280, 666). Toggles the Camera360Screen
    // overlay; z is above that overlay (600) so the icon stays the tap
    // target that closes it, rather than getting covered once it's open.
    // Hidden while the diagnostic screen or camera grid screen is
    // open/opening — it lives at the same on-screen spot as diagnostic
    // content, and it's the wrong action to expose over either screen anyway.
    Image {
        id: icon360
        x: 280 - width / 2; y: 666 - height / 2
        z: 700
        visible: !diagnosticScreen.isOpen && !cameraGridScreen.isOpen
        source: "qrc:/icon_360.png"
        opacity: icon360Area.pressed ? 0.6 : 1.0

        MouseArea {
            id: icon360Area
            anchors.fill: parent
            anchors.margins: -10
            onClicked: camera360Screen.visible ? camera360Screen.close() : camera360Screen.open()
        }
    }

    // Page indicator — bottom center, shows main as the middle of the
    // 3-screen swipe layout (camera grid left, diagnostics right). Hidden
    // while either overlay screen is open/opening, same as icon360 above —
    // it lives at a spot those screens' own indicators cover once open, and
    // showing it mid-swipe would double up with the incoming screen's dots.
    PageIndicator {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 696
        z: 150
        currentIndex: 1
        visible: !diagnosticScreen.isOpen && !cameraGridScreen.isOpen
    }

    // Top indicator row — evenly spaced at 80px intervals, centered at x=800.
    // Oil/battery/coolant are true warnings, so once startup is done they
    // also gate on _warnFlash to blink at 300ms; check engine and the beam
    // indicators are steady state lamps, not warnings, so they just track
    // their sim property directly.
    Image {
        x: 640 - width / 2; y: 23
        source: "qrc:/icon_oil_pressure.png"
        visible: startupActive ? startupFlash : (sim.oilPressureWarn && _warnFlash)
    }
    Image {
        x: 720 - width / 2; y: 23
        source: "qrc:/icon_check_engine.png"
        visible: startupActive ? startupFlash : sim.checkEngine
    }
    Image {
        x: 800 - width / 2; y: 23
        source: "qrc:/icon_low_beam.png"
        visible: startupActive ? startupFlash : sim.lowBeams
    }
    Image {
        x: 800 - width / 2; y: 23
        source: "qrc:/icon_high_beam.png"
        visible: startupActive ? startupFlash : sim.highBeams
    }
    Image {
        x: 880 - width / 2; y: 23
        source: "qrc:/icon_battery.png"
        visible: startupActive ? startupFlash : (sim.batteryWarn && _warnFlash)
    }
    Image {
        x: 960 - width / 2; y: 23
        source: "qrc:/icon_coolant_warn.png"
        visible: startupActive ? startupFlash : (sim.coolantWarn && _warnFlash)
    }

    // Fonts
    FontLoader {
        id: rangeFont
        source: "qrc:/range.regular.ttf"
    }
    FontLoader {
        id: bahnschriftFont
        source: "qrc:/bahnschrift._semibold.ttf"
    }

    // Gear indicator — centered at (798, 631)
    Text {
        id: gearIndicator
        x: 803 - width / 2
        y: 309 - height / 2
        font.family: bahnschriftFont.name
        font.pixelSize: 150
        color: "white"
        text: {
            var g = sim.gear
            if (g === -2) return "P"
            if (g === -1) return "R"
            if (g === 0) return "N"
            return g.toString()
        }
    }

    // Widest glyph the gear indicator can show ("N", wider than "P"/"R" or
    // any digit in this font) — used to give the transmission badge a fixed
    // position instead of one that shifts with the current gear text's width.
    TextMetrics {
        id: gearWidthMetric
        font.family: bahnschriftFont.name
        font.pixelSize: gearIndicator.font.pixelSize
        text: "N"
    }

    // Baselines for the two font sizes — aligning bounding-box bottoms would
    // leave the badge sitting visibly lower than the gear glyph, since a
    // 150px font's descent eats far more of its box than a 32px font's does.
    FontMetrics {
        id: gearFontMetrics
        font.family: bahnschriftFont.name
        font.pixelSize: gearIndicator.font.pixelSize
    }
    FontMetrics {
        id: transmissionBadgeMetrics
        font.family: bahnschriftFont.name
        font.pixelSize: 32
    }

    // Transmission mode badge — fixed to the right of the gear indicator,
    // baseline-aligned with it.
    Text {
        x: 813 + gearWidthMetric.width / 2
        y: gearIndicator.y + gearFontMetrics.ascent - transmissionBadgeMetrics.ascent
        font.family: bahnschriftFont.name
        font.pixelSize: 32
        color: "white"
        text: sim.transmissionAuto ? "A" : "M"
    }

    // Drive mode indicator — top-left corner at (1087, 614)
    Text {
        x: 1087
        y: 614
        font.family: bahnschriftFont.name
        font.pixelSize: 28
        text: sim.driveMode
        color: {
            if (sim.driveMode === "RACE") return "red"
            if (sim.driveMode === "SPORT+") return "orange"
            return "white"
        }
    }

    // Clock — mirrors the drive mode indicator's spot on the left side,
    // right-aligned against the mirror of its x=1087 left edge (center 800).
    Text {
        id: clockText
        property date now: new Date()
        x: 513 - width
        y: 614
        font.family: bahnschriftFont.name
        font.pixelSize: 28
        color: "white"
        text: Qt.formatDateTime(now, "h:mm AP")

        Timer {
            interval: 1000
            running: true
            repeat: true
            onTriggered: clockText.now = new Date()
        }

        MouseArea {
            anchors.fill: parent
            anchors.margins: -10
            onClicked: setTimeScreen.open()
        }
    }

    // Odometer — "ODO  x mi", centered on x=660 (recomputed from width on
    // every text change) so it grows evenly outward as the digit count
    // changes instead of drifting to one side.
    Text {
        id: odoText
        x: 660 - width / 2
        y: 607
        font.family: rangeFont.name
        font.pixelSize: 24
        color: "white"
        text: "ODO  " + sim.totalOdo.toFixed(1) + " mi"
    }

    // Trip odometer — right-aligned against the "mi" label at x=1013
    Text {
        id: tripText
        x: 1001 - width
        y: 617
        font.family: rangeFont.name
        font.pixelSize: 24
        color: "white"
        text: sim.tripOdo.toFixed(1)
    }

    // Trip reset button - a circular-arrow icon drawn on a Canvas rather
    // than the U+21BA Text glyph this used to be. Neither bundled font
    // (bahnschrift, range) contains that glyph, and this app has no
    // guaranteed system fallback font to borrow it from (BeaglePlay's
    // Yocto image ships none at all), so it rendered as a missing-glyph
    // box on target. Canvas is proven to render here already (see
    // boostRing above, same QT_QUICK_BACKEND=software path).
    Item {
        id: tripReset
        x: 1040
        y: tripText.y - 2
        width: 26
        height: 26
        z: 200

        Canvas {
            id: resetIcon
            anchors.fill: parent
            property color strokeColor: tripResetArea.pressed ? "#ffffff" : "#aaaaaa"
            onStrokeColorChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                var cx = width / 2
                var cy = height / 2
                var r = width / 2 - 4

                // Arc sweeps clockwise the long way from 9 o'clock round to
                // 6 o'clock, leaving a gap in the bottom-left quadrant.
                // Ending exactly at 6 o'clock (PI/2) means the direction of
                // travel there is straight left - an axis-aligned arrowhead,
                // no trig needed for its shape.
                ctx.strokeStyle = strokeColor
                ctx.lineWidth = 2.5
                ctx.lineCap = "round"
                ctx.beginPath()
                ctx.arc(cx, cy, r, Math.PI, Math.PI / 2, false)
                ctx.stroke()

                var tipX = cx - r
                var tipY = cy + r
                var headLen = 6
                var headWidth = 7
                ctx.fillStyle = strokeColor
                ctx.beginPath()
                ctx.moveTo(tipX - headLen, tipY)
                ctx.lineTo(tipX + headLen * 0.6, tipY - headWidth / 2)
                ctx.lineTo(tipX + headLen * 0.6, tipY + headWidth / 2)
                ctx.closePath()
                ctx.fill()
            }
        }

        MouseArea {
            id: tripResetArea
            anchors.fill: parent
            anchors.margins: -6
            onClicked: {
                sim.tripOdo = 0
                sim.save()
            }
        }
    }

    // Touch feedback dot
    Rectangle {
        id: touchDot
        width: 30
        height: 30
        radius: 15
        color: "#00ffff"
        opacity: 0
        z: 100

        SequentialAnimation {
            id: touchAnim
            PropertyAction { target: touchDot; property: "opacity"; value: 1.0 }
            PropertyAction { target: touchDot; property: "scale"; value: 0.5 }
            ParallelAnimation {
                NumberAnimation { target: touchDot; property: "scale"; to: 2.0; duration: 400; easing.type: Easing.OutQuad }
                NumberAnimation { target: touchDot; property: "opacity"; to: 0; duration: 400; easing.type: Easing.OutQuad }
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        // Stacked below interactive elements (odometer/trip drag handles,
        // trip reset button) so it doesn't swallow their presses — it only
        // catches touches that land on non-interactive parts of the dash.
        // Also doubles as the swipe gesture into the diagnostic screen
        // (swipe left, see DiagnosticScreen.qml) and the camera grid screen
        // (swipe right, see CameraGridScreen.qml) — same layer, same
        // primitive, rather than adding a second overlay convention just for
        // gestures.
        z: -1
        property real dragStartX: 0
        property bool swipeFired: false
        onPressed: {
            dragStartX = mouse.x
            swipeFired = false
            touchDot.x = mouse.x - touchDot.width / 2
            touchDot.y = mouse.y - touchDot.height / 2
            touchAnim.restart()
        }
        // Fires the swipe as soon as the threshold is crossed mid-drag,
        // rather than waiting for onReleased — a plain MouseArea's release
        // isn't reliably delivered here once qtquick treats a press-then-move
        // as a drag, presumably related to its own mouse-to-touch gesture
        // synthesis stealing the ungrab (confirmed via synthetic-drag testing
        // on the macOS dev build; not confirmed against real touch hardware,
        // worth rechecking on the board). Triggering on the move that
        // crosses the threshold sidesteps depending on release firing at all.
        onPositionChanged: {
            if (startupActive || swipeFired) return
            if (mouse.x - dragStartX < -120) {
                swipeFired = true
                diagnosticScreen.open()
            } else if (mouse.x - dragStartX > 120) {
                swipeFired = true
                cameraGridScreen.open()
            }
        }
    }

    // Time-set overlay — opened by tapping the clock. Stacked above
    // everything else (including the touch feedback dot at z=100).
    SetTimeScreen {
        id: setTimeScreen
    }

    // Diagnostic screen — opened by swiping left anywhere on the dash (see
    // the MouseArea above). Stacked above the touch dot/trip reset but below
    // the time-set overlay.
    DiagnosticScreen {
        id: diagnosticScreen
    }

    // Camera grid screen — opened by swiping right anywhere on the dash (see
    // the MouseArea above). Same z-tier as diagnosticScreen since the two
    // are mutually exclusive swipe targets off the main cluster.
    CameraGridScreen {
        id: cameraGridScreen
    }

    // 360-degree camera view — opened/closed by tapping icon360 above, or
    // automatically on reverse gear, like a real backup camera.
    // Stacked above the time-set overlay; icon360 itself sits above this.
    Camera360Screen {
        id: camera360Screen
    }

    // Reverse-gear auto-open/close for camera360Screen above. The
    // !startupActive guard is load-bearing: the ~2s startup self-test sweeps
    // gear through every value (see gearIndicator's binding above), which
    // would otherwise pop the camera overlay open on every single boot.
    property bool reverseGear: !startupActive && sim.gear === -1
    onReverseGearChanged: reverseGear ? camera360Screen.open() : camera360Screen.close()

    // FPS overlay — top-left corner, above every screen this cluster shows
    // (diagnostic, camera grid, 360 view + its calibration panel, time-set)
    // so a rendering slowdown is visible no matter what's on screen. Below
    // only the boot splash (9999), which shouldn't show performance chrome.
    Text {
        id: fpsText
        x: 8
        y: 4
        z: 9000
        font.family: bahnschriftFont.name
        font.pixelSize: 16
        color: "#00ff00"
        text: "-- FPS"

        Timer {
            interval: 1000
            running: true
            repeat: true
            onTriggered: {
                fpsText.text = root._fpsFrameCount + " FPS"
                root._fpsFrameCount = 0
            }
        }
    }

    // Boot splash overlay — see splashDone above. Declared last / z above
    // everything so it fully hides the dash (already mid-self-test
    // underneath) until the two Timers above dismiss it. No-op, not even
    // created visibly, when splashImagePath is empty.
    Rectangle {
        id: bootOverlay
        anchors.fill: parent
        color: "black"
        z: 9999
        visible: splashImagePath !== ""

        Image {
            id: bootSplashImage
            anchors.fill: parent
            source: splashImagePath
            visible: false
            fillMode: Image.PreserveAspectFit
        }
    }
}
