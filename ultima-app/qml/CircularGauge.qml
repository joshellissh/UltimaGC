import QtQuick 2.15

Item {
    id: gauge

    property real value: 0
    property real minValue: 0
    property real maxValue: 220

    property real startAngle: 135
    property real endAngle: 405
    property bool counterClockwise: false

    // Needle size and pivot point within the needle image (from top-left)
    property real needleWidth: 98
    property real needleHeight: 350
    property real pivotX: 48
    property real pivotY: 259

    property bool debug: false

    property real displayValue: value
    Behavior on displayValue {
        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
    }

    // Debug: draw sweep arc and min/max markers
    Canvas {
        id: debugCanvas
        anchors.fill: parent
        visible: gauge.debug
        z: 10

        onPaint: {
            var ctx = getContext("2d")
            var w = width
            var h = height
            var cx = w / 2
            var cy = h / 2
            var radius = Math.min(cx, cy) * 0.7

            ctx.clearRect(0, 0, w, h)

            function toRad(deg) {
                return (deg - 90) * Math.PI / 180
            }

            // Draw sweep arc
            ctx.beginPath()
            ctx.arc(cx, cy, radius, toRad(startAngle), toRad(endAngle), counterClockwise)
            ctx.strokeStyle = "rgba(0, 255, 0, 0.6)"
            ctx.lineWidth = 3
            ctx.stroke()

            // Start marker (green = min value)
            var sRad = toRad(startAngle)
            ctx.beginPath()
            ctx.moveTo(cx + (radius - 15) * Math.cos(sRad), cy + (radius - 15) * Math.sin(sRad))
            ctx.lineTo(cx + (radius + 15) * Math.cos(sRad), cy + (radius + 15) * Math.sin(sRad))
            ctx.strokeStyle = "lime"
            ctx.lineWidth = 3
            ctx.stroke()

            // End marker (red = max value)
            var eRad = toRad(endAngle)
            ctx.beginPath()
            ctx.moveTo(cx + (radius - 15) * Math.cos(eRad), cy + (radius - 15) * Math.sin(eRad))
            ctx.lineTo(cx + (radius + 15) * Math.cos(eRad), cy + (radius + 15) * Math.sin(eRad))
            ctx.strokeStyle = "red"
            ctx.lineWidth = 3
            ctx.stroke()

            // Label
            ctx.fillStyle = "lime"
            ctx.font = "12px sans-serif"
            var dir = counterClockwise ? " CCW" : " CW"
            ctx.fillText(startAngle + "° → " + endAngle + "°" + dir, 5, 15)
        }

        Connections {
            target: gauge
            function onStartAngleChanged() { debugCanvas.requestPaint() }
            function onEndAngleChanged() { debugCanvas.requestPaint() }
        }
    }

    Image {
        id: needleImage
        source: "qrc:/needle.png"
        width: gauge.needleWidth
        height: gauge.needleHeight

        // Position so pivot point sits at gauge center
        x: parent.width / 2 - pivotX
        y: parent.height / 2 - pivotY

        antialiasing: true
        smooth: true

        transform: Rotation {
            origin.x: pivotX
            origin.y: pivotY
            angle: {
                var frac = (displayValue - minValue) / (maxValue - minValue)
                frac = Math.max(0, Math.min(1, frac))
                if (counterClockwise) {
                    return startAngle - frac * (startAngle - endAngle)
                } else {
                    return startAngle + frac * (endAngle - startAngle)
                }
            }
        }
    }
}
