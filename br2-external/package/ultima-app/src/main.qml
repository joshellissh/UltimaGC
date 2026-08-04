import QtQuick 2.15
import QtQuick.Window 2.15

Window {
    id: root
    width: 1600
    height: 720
    visibility: Window.Windowed
    color: "black"

    // Background layers, back to front: boost circle, gauge overlay, car
    //
    // The boost ring wraps the tachometer (same center/sweep as rpmGauge:
    // 1251,343 / 270deg-503deg CW). It's fully covered by a pie wedge at
    // zero boost, radially exposing more of the ring as boost builds —
    // sweeping in the same direction as the tach's own needle travel.
    Canvas {
        id: boostRing
        anchors.fill: parent

        readonly property real centerX: 1251
        readonly property real centerY: 343
        readonly property real startAngle: 270
        readonly property real endAngle: 503
        readonly property real maxBoost: 30

        property real displayBoost: sim.boost
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
            ctx.drawImage("qrc:/boost_circle.png", 0, 0, width, height)
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
    }

    // Periodic odometer save (every 30s) — CanBus owns integration and pushes
    // the latest values into OdoStore.
    Timer {
        interval: 30000
        running: true
        repeat: true
        onTriggered: sim.save()
    }

    // Left gauge: Speedometer — pivot at (351, 342)
    CircularGauge {
        id: speedGauge
        x: 351 - width / 2
        y: 342 - height / 2
        width: 600
        height: 600
        value: sim.speed
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
        value: sim.rpm / 1000
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
        value: sim.fuelLevel
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
        value: sim.coolantTemp
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
        visible: true
    }

    // Right turn signal indicator (mirrored)
    Image {
        x: 1600 - 25 - width
        y: 23
        source: "qrc:/left_indicator.png"
        visible: true
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
        visible: true
    }

    // Top indicator row — evenly spaced at 80px intervals, centered at x=800
    Image {
        x: 640 - width / 2; y: 23
        source: "qrc:/icon_oil_pressure.png"
        visible: true
    }
    Image {
        x: 720 - width / 2; y: 23
        source: "qrc:/icon_check_engine.png"
        visible: true
    }
    Image {
        x: 800 - width / 2; y: 23
        source: "qrc:/icon_low_beam.png"
        visible: true
    }
    Image {
        x: 800 - width / 2; y: 23
        source: "qrc:/icon_high_beam.png"
        visible: true
    }
    Image {
        x: 880 - width / 2; y: 23
        source: "qrc:/icon_battery.png"
        visible: true
    }
    Image {
        x: 960 - width / 2; y: 23
        source: "qrc:/icon_coolant_warn.png"
        visible: true
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

    // Gear indicator — centered at (798, 601)
    Text {
        id: gearIndicator
        x: 803 - width / 2
        y: 279 - height / 2
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

    // Trip reset button
    Text {
        x: 1040
        y: tripText.y
        font.pixelSize: 24
        color: tripResetArea.pressed ? "#ffffff" : "#aaaaaa"
        text: "\u21BA"
        z: 200

        MouseArea {
            id: tripResetArea
            anchors.fill: parent
            anchors.margins: -10
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
        z: -1
        onPressed: {
            touchDot.x = mouse.x - touchDot.width / 2
            touchDot.y = mouse.y - touchDot.height / 2
            touchAnim.restart()
        }
    }
}
