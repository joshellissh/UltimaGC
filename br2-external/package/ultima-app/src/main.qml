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

    // background.png bakes in an orange "road" gradient under this area — it
    // used to be masked by the old boost trapezoid's black overlay. Cover it
    // solidly now that that mechanism is gone.
    Rectangle {
        x: 350
        y: 460
        width: 897
        height: 207
        color: "black"
    }

    // Boost gauge — small pill matching the fuel/coolant dial style
    BoostGauge {
        id: boostGauge
        x: 620 - width / 2
        y: 560 - height / 2
        value: sim.boost
        fontFamily: rangeFont.name
    }

    // Shift-light bar — fills with rpm, blinks red together at redline
    ShiftLightBar {
        x: 760
        y: 560 - height / 2
        width: 420
        height: 36
        rpm: sim.rpm
        redlineRpm: shiftRedlineRpm
        flash: _warnFlash
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

    // Warning flash timer (300ms cycle) — also drives the shift-light blink at redline
    property bool _warnFlash: true
    property real shiftRedlineRpm: 7000
    Timer {
        interval: 300
        running: sim.oilPressureWarn || sim.batteryWarn || sim.coolantWarn || sim.rpm >= shiftRedlineRpm
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
