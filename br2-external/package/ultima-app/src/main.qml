import QtQuick 2.15
import QtQuick.Window 2.15

Window {
    id: root
    width: 1600
    height: 720
    visibility: Window.Windowed
    color: "black"

    // Background image
    Image {
        anchors.fill: parent
        source: "qrc:/background.png"
        fillMode: Image.PreserveAspectCrop
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

    // Boost gauge — trapezoid black overlay that recedes upward with boost
    Canvas {
        x: 350
        y: 460
        width: 897   // 1247 - 350
        height: 207  // 667 - 460

        property real boost: sim.boost
        onBoostChanged: requestPaint()

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)

            var f = Math.min(1, Math.max(0, boost / 30))
            var h = height * (1 - f)  // black area height from top
            if (h <= 0) return

            // Trapezoid edges: top is narrow (350..554), bottom is wide (0..897)
            var tlx = 350  // 700 - 350
            var trx = 554  // 904 - 350
            var t = h / height
            var blx = tlx * (1 - t)
            var brx = trx + t * (897 - trx)

            ctx.fillStyle = "black"
            ctx.beginPath()
            ctx.moveTo(tlx, 0)
            ctx.lineTo(trx, 0)
            ctx.lineTo(brx, h)
            ctx.lineTo(blx, h)
            ctx.closePath()
            ctx.fill()
        }
    }

    // Boost PSI readout
    Text {
        x: 800 - width / 2
        y: 475 - height / 2
        z: 10
        font.family: rangeFont.name
        font.pixelSize: 22
        color: "white"
        text: Math.round(sim.boost) + " PSI"
    }

    // Boost gauge scale lines overlay (above black trapezoid)
    Image {
        x: 0
        y: 0
        z: 1
        source: "qrc:/boost_lines_overlay.png"
    }

    // Left turn signal indicator
    Image {
        x: 25
        y: 23
        source: "qrc:/left_indicator.png"
        visible: sim.leftIndicator
    }

    // Right turn signal indicator (mirrored)
    Image {
        x: 1600 - 25 - width
        y: 23
        source: "qrc:/left_indicator.png"
        visible: sim.rightIndicator
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

    // Top indicator row — evenly spaced at 80px intervals, centered at x=800
    Image {
        x: 640 - width / 2; y: 23
        source: "qrc:/icon_oil_pressure.png"
        visible: sim.oilPressureWarn && _warnFlash
    }
    Image {
        x: 720 - width / 2; y: 23
        source: "qrc:/icon_check_engine.png"
        visible: sim.checkEngine
    }
    Image {
        x: 800 - width / 2; y: 23
        source: "qrc:/icon_low_beam.png"
        visible: sim.lowBeams && !sim.highBeams
    }
    Image {
        x: 800 - width / 2; y: 23
        source: "qrc:/icon_high_beam.png"
        visible: sim.highBeams
    }
    Image {
        x: 880 - width / 2; y: 23
        source: "qrc:/icon_battery.png"
        visible: sim.batteryWarn && _warnFlash
    }
    Image {
        x: 960 - width / 2; y: 23
        source: "qrc:/icon_coolant_warn.png"
        visible: sim.coolantWarn && _warnFlash
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

    // Odometer (left of center)
    Text {
        x: 450 - width / 2
        y: 670
        font.family: rangeFont.name
        font.pixelSize: 32
        color: "#aaaaaa"
        text: sim.totalOdo.toFixed(1) + " mi"
    }

    // Trip odometer (right of center)
    Text {
        id: tripText
        x: 1150 - width / 2
        y: 670
        font.family: rangeFont.name
        font.pixelSize: 32
        color: "#aaaaaa"
        text: "TRIP  " + sim.tripOdo.toFixed(1) + " mi"
    }

    // Trip reset button
    Text {
        x: tripText.x + tripText.width + 8
        y: tripText.y
        font.pixelSize: 32
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
        onPressed: {
            touchDot.x = mouse.x - touchDot.width / 2
            touchDot.y = mouse.y - touchDot.height / 2
            touchAnim.restart()
        }
    }
}
