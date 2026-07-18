import QtQuick 2.15

// Small boost pill gauge, styled to match the fuel/coolant dials on
// background.png (thin white arc, bracket end-caps, radial ticks). Unlike
// those two, background.png has no printed face for boost, so this draws
// its own face at runtime instead of relying on baked-in artwork.
Item {
    id: gauge
    width: 200
    height: 200

    property real value: 0
    property real minValue: 0
    property real maxValue: 30
    property real startAngle: 217
    property real endAngle: 307.5
    property string fontFamily: ""

    property real displayValue: value
    Behavior on displayValue {
        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
    }

    function toRad(deg) { return (deg - 90) * Math.PI / 180 }

    Canvas {
        id: face
        anchors.fill: parent

        property real radius: 68
        property real attentionFrac: 0.83 // ~25 psi: where the amber/red tint starts

        Connections {
            target: gauge
            function onDisplayValueChanged() { face.requestPaint() }
        }

        onPaint: {
            var ctx = getContext("2d")
            var cx = width / 2, cy = height / 2
            ctx.clearRect(0, 0, width, height)

            function angleAt(frac) {
                return toRad(startAngle + frac * (endAngle - startAngle))
            }
            function point(frac, r) {
                var a = angleAt(frac)
                return { x: cx + r * Math.cos(a), y: cy + r * Math.sin(a) }
            }

            // Base arc (0 .. attention zone)
            ctx.strokeStyle = "#ffffff"
            ctx.lineWidth = 3
            ctx.beginPath()
            ctx.arc(cx, cy, radius, angleAt(0), angleAt(attentionFrac))
            ctx.stroke()

            // Attention-zone arc, amber -> red toward max
            var p0 = point(attentionFrac, radius)
            var p1 = point(1, radius)
            var grad = ctx.createLinearGradient(p0.x, p0.y, p1.x, p1.y)
            grad.addColorStop(0, "#ff9900")
            grad.addColorStop(1, "#ff2020")
            ctx.strokeStyle = grad
            ctx.beginPath()
            ctx.arc(cx, cy, radius, angleAt(attentionFrac), angleAt(1))
            ctx.stroke()

            // Ticks every 5 psi; major (bigger) every 15 psi; brackets at the ends
            var minorStep = 5
            var majorStep = 15
            for (var v = minValue; v <= maxValue + 0.01; v += minorStep) {
                var f = (v - minValue) / (maxValue - minValue)
                var isEnd = (v === minValue || v === maxValue)
                var isMajor = (Math.round((v - minValue) / majorStep) * majorStep === Math.round(v - minValue))
                var inner = radius - (isEnd ? 14 : (isMajor ? 10 : 6))
                var outer = radius + (isEnd ? 6 : 2)
                var pIn = point(f, inner)
                var pOut = point(f, outer)

                ctx.strokeStyle = "#ffffff"
                ctx.lineWidth = isEnd ? 4 : (isMajor ? 3 : 2)
                ctx.beginPath()
                ctx.moveTo(pIn.x, pIn.y)
                ctx.lineTo(pOut.x, pOut.y)
                ctx.stroke()

                if (isEnd) {
                    // Short tangential stroke turns the radial tick into a bracket
                    var a = angleAt(f)
                    var tx = -Math.sin(a), ty = Math.cos(a)
                    var dir = (v === minValue) ? 1 : -1
                    ctx.beginPath()
                    ctx.moveTo(pOut.x, pOut.y)
                    ctx.lineTo(pOut.x + tx * 10 * dir, pOut.y + ty * 10 * dir)
                    ctx.stroke()
                }
            }

            // Major labels at min / mid / max
            ctx.fillStyle = "#ffffff"
            ctx.font = "16px " + fontFamily
            ctx.textAlign = "center"
            ctx.textBaseline = "middle"
            var labelFracs = [0, 0.5, 1]
            for (var i = 0; i < labelFracs.length; i++) {
                var lf = labelFracs[i]
                var lv = Math.round(minValue + lf * (maxValue - minValue))
                var lp = point(lf, radius + 18)
                ctx.fillText(lv.toString(), lp.x, lp.y)
            }

            // Live readout + unit caption, centered in the dial's open lower area
            ctx.font = "26px " + fontFamily
            ctx.fillStyle = "#ffffff"
            ctx.fillText(Math.round(displayValue).toString(), cx, cy + radius * 0.55)
            ctx.font = "11px " + fontFamily
            ctx.fillStyle = "#aaaaaa"
            ctx.fillText("PSI", cx, cy + radius * 0.55 + 20)
        }
    }

    Image {
        id: needleImage
        source: "qrc:/needle.png"
        width: 28
        height: 100

        x: parent.width / 2 - 14
        y: parent.height / 2 - 74

        antialiasing: true
        smooth: true

        transform: Rotation {
            origin.x: 14
            origin.y: 74
            angle: {
                var frac = (displayValue - minValue) / (maxValue - minValue)
                frac = Math.max(0, Math.min(1, frac))
                return startAngle + frac * (endAngle - startAngle)
            }
        }
    }
}
