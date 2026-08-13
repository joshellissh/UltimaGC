import QtQuick 2.15
import QtQuick.Window 2.15

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
    Image {
        anchors.fill: parent
        source: "qrc:/car_lights_off.png"
        visible: !sim.lowBeams && !sim.highBeams
    }
    Image {
        anchors.fill: parent
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

    // Left turn signal indicator
    Image {
        x: 25
        y: 23
        source: "qrc:/left_indicator.png"
        visible: startupActive ? startupFlash : sim.leftIndicator
    }

    // Right turn signal indicator (mirrored)
    Image {
        x: 1600 - 25 - width
        y: 23
        source: "qrc:/left_indicator.png"
        visible: startupActive ? startupFlash : sim.rightIndicator
        mirror: true
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

    // Odometer — right-aligned against the "mi" label at x=772
    Text {
        id: odoText
        x: 760 - width
        y: 617
        font.family: rangeFont.name
        font.pixelSize: 24
        color: "white"
        text: sim.totalOdo.toFixed(1)
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
        // Also doubles as the swipe-left gesture into the diagnostic screen
        // (see DiagnosticScreen.qml) — same layer, same primitive, rather
        // than adding a second overlay convention just for gestures.
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
