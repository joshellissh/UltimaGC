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

    SimEngine {
        id: sim
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

    // Beam indicators — top center (shared position)
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

    // Dashboard warning indicators — row centered at y=495
    Image {
        x: 720 - width / 2; y: 495 - height / 2
        source: "qrc:/icon_oil_pressure.png"
        visible: sim.oilPressureWarn
    }
    Image {
        x: 780 - width / 2; y: 495 - height / 2
        source: "qrc:/icon_check_engine.png"
        visible: sim.checkEngine
    }
    Image {
        x: 840 - width / 2; y: 495 - height / 2
        source: "qrc:/icon_battery.png"
        visible: sim.batteryWarn
    }
    Image {
        x: 900 - width / 2; y: 495 - height / 2
        source: "qrc:/icon_coolant_warn.png"
        visible: sim.coolantWarn
    }

    // Gear indicator — centered at (798, 601)
    FontLoader {
        id: rangeFont
        source: "qrc:/range.regular.ttf"
    }

    Text {
        id: gearIndicator
        x: 798 - width / 2
        y: 586 - height / 2
        font.family: rangeFont.name
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
            onClicked: sim.tripOdo = 0
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
