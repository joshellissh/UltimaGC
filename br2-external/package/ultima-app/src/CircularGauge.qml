import QtQuick 2.15

Item {
    id: gauge

    property real value: 0
    property real minValue: 0
    property real maxValue: 220

    property real startAngle: 135
    property real endAngle: 405

    property color arcColor: "#FF8C00"
    property color needleColor: arcColor
    property color tickColor: "white"
    property color textColor: "white"
    property color warningColor: "#FF2020"
    property real warningStart: -1

    property string unitLabel: "km/h"
    property var majorTicks: []
    property int minorTickCount: 4

    property real displayValue: value
    Behavior on displayValue {
        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
    }

    // Repaint on value change
    onDisplayValueChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Threaded

        onPaint: {
            var ctx = getContext("2d")
            var w = width
            var h = height
            var cx = w / 2
            var cy = h / 2
            var radius = Math.min(cx, cy) * 0.88

            ctx.clearRect(0, 0, w, h)

            function toRad(gaugeDeg) {
                return (gaugeDeg - 90) * Math.PI / 180
            }
            function valueToAngle(v) {
                var frac = (v - minValue) / (maxValue - minValue)
                return startAngle + Math.max(0, Math.min(1, frac)) * (endAngle - startAngle)
            }

            // --- Gauge face: subtle radial gradient for depth ---
            var faceGrad = ctx.createRadialGradient(cx, cy, radius * 0.1, cx, cy, radius * 1.05)
            faceGrad.addColorStop(0, "#1a1a1a")
            faceGrad.addColorStop(0.6, "#111111")
            faceGrad.addColorStop(1.0, "#080808")
            ctx.beginPath()
            ctx.arc(cx, cy, radius * 1.02, 0, Math.PI * 2)
            ctx.fillStyle = faceGrad
            ctx.fill()

            // --- Outer bezel ring ---
            ctx.beginPath()
            ctx.arc(cx, cy, radius * 1.03, 0, Math.PI * 2)
            ctx.strokeStyle = "#2a2a2a"
            ctx.lineWidth = 2
            ctx.stroke()

            // --- Background arc track (subtle) ---
            ctx.beginPath()
            ctx.arc(cx, cy, radius, toRad(startAngle), toRad(endAngle))
            ctx.strokeStyle = "#222222"
            ctx.lineWidth = 16
            ctx.lineCap = "butt"
            ctx.stroke()

            // --- Warning zone (subtle tinted background) ---
            if (warningStart >= minValue && warningStart < maxValue) {
                ctx.beginPath()
                ctx.arc(cx, cy, radius, toRad(valueToAngle(warningStart)), toRad(endAngle))
                ctx.strokeStyle = Qt.rgba(1, 0.1, 0.1, 0.15)
                ctx.lineWidth = 16
                ctx.stroke()
            }

            // --- Value arc with GLOW ---
            var valueAngle = valueToAngle(displayValue)
            if (displayValue > minValue) {
                // Outer glow (wide, transparent)
                ctx.beginPath()
                ctx.arc(cx, cy, radius, toRad(startAngle), toRad(valueAngle))
                var r = parseInt(arcColor.toString().slice(1,3), 16) / 255
                var g = parseInt(arcColor.toString().slice(3,5), 16) / 255
                var b = parseInt(arcColor.toString().slice(5,7), 16) / 255
                ctx.strokeStyle = Qt.rgba(r, g, b, 0.08)
                ctx.lineWidth = 40
                ctx.lineCap = "butt"
                ctx.stroke()

                // Mid glow
                ctx.beginPath()
                ctx.arc(cx, cy, radius, toRad(startAngle), toRad(valueAngle))
                ctx.strokeStyle = Qt.rgba(r, g, b, 0.2)
                ctx.lineWidth = 24
                ctx.stroke()

                // Core arc
                ctx.beginPath()
                ctx.arc(cx, cy, radius, toRad(startAngle), toRad(valueAngle))
                ctx.strokeStyle = arcColor
                ctx.lineWidth = 8
                ctx.stroke()

                // Bright inner edge
                ctx.beginPath()
                ctx.arc(cx, cy, radius, toRad(startAngle), toRad(valueAngle))
                ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.25)
                ctx.lineWidth = 2
                ctx.stroke()
            }

            // --- Tick marks ---
            var majorLen = radius * 0.13
            var minorLen = radius * 0.06
            var labelRadius = radius - majorLen - 22

            for (var i = 0; i < majorTicks.length; i++) {
                var tickVal = majorTicks[i]
                var tickAngle = valueToAngle(tickVal)
                var tickRad = toRad(tickAngle)
                var cos_t = Math.cos(tickRad)
                var sin_t = Math.sin(tickRad)

                var inWarning = (warningStart >= minValue && tickVal >= warningStart)
                // Ticks that the value arc has passed glow
                var isLit = (tickVal <= displayValue)

                // Major tick
                ctx.beginPath()
                ctx.moveTo(cx + (radius + 5) * cos_t, cy + (radius + 5) * sin_t)
                ctx.lineTo(cx + (radius - majorLen) * cos_t, cy + (radius - majorLen) * sin_t)
                if (inWarning) {
                    ctx.strokeStyle = isLit ? "#FF4040" : Qt.rgba(1, 0.2, 0.2, 0.5)
                } else {
                    ctx.strokeStyle = isLit ? Qt.rgba(1, 1, 1, 0.95) : Qt.rgba(1, 1, 1, 0.35)
                }
                ctx.lineWidth = 2.5
                ctx.stroke()

                // Tick glow when lit
                if (isLit && !inWarning) {
                    ctx.beginPath()
                    ctx.moveTo(cx + (radius + 5) * cos_t, cy + (radius + 5) * sin_t)
                    ctx.lineTo(cx + (radius - majorLen) * cos_t, cy + (radius - majorLen) * sin_t)
                    ctx.strokeStyle = Qt.rgba(r || 1, g || 0.55, b || 0, 0.3)
                    ctx.lineWidth = 6
                    ctx.stroke()
                }

                // Label
                ctx.save()
                ctx.fillStyle = inWarning ? Qt.rgba(1, 0.3, 0.3, isLit ? 1.0 : 0.6) :
                                            Qt.rgba(1, 1, 1, isLit ? 0.9 : 0.4)
                ctx.font = (isLit ? "bold " : "") + "15px 'Helvetica Neue', Helvetica, Arial, sans-serif"
                ctx.textAlign = "center"
                ctx.textBaseline = "middle"
                ctx.fillText(tickVal.toString(), cx + labelRadius * cos_t, cy + labelRadius * sin_t)
                ctx.restore()

                // Minor ticks
                if (i < majorTicks.length - 1) {
                    var nextVal = majorTicks[i + 1]
                    var step = (nextVal - tickVal) / (minorTickCount + 1)
                    for (var m = 1; m <= minorTickCount; m++) {
                        var minorVal = tickVal + step * m
                        var minorRad = toRad(valueToAngle(minorVal))
                        var mcos = Math.cos(minorRad)
                        var msin = Math.sin(minorRad)
                        var minorLit = (minorVal <= displayValue)
                        ctx.beginPath()
                        ctx.moveTo(cx + (radius + 2) * mcos, cy + (radius + 2) * msin)
                        ctx.lineTo(cx + (radius - minorLen) * mcos, cy + (radius - minorLen) * msin)
                        ctx.strokeStyle = minorLit ? Qt.rgba(1, 1, 1, 0.5) : Qt.rgba(1, 1, 1, 0.15)
                        ctx.lineWidth = 1
                        ctx.stroke()
                    }
                }
            }

            // --- Inner decorative ring ---
            var innerR = radius * 0.48
            ctx.beginPath()
            ctx.arc(cx, cy, innerR, toRad(startAngle), toRad(endAngle))
            ctx.strokeStyle = Qt.rgba(0, 0.808, 0.82, 0.12)
            ctx.lineWidth = 1
            ctx.stroke()

            // Second subtle inner ring
            ctx.beginPath()
            ctx.arc(cx, cy, innerR * 0.92, toRad(startAngle + 10), toRad(endAngle - 10))
            ctx.strokeStyle = Qt.rgba(0, 0.808, 0.82, 0.06)
            ctx.lineWidth = 0.5
            ctx.stroke()

            // --- Needle shadow ---
            var needleRad = toRad(valueAngle)
            var needleLen = radius * 0.78
            var needleTail = radius * 0.18
            var ncos = Math.cos(needleRad)
            var nsin = Math.sin(needleRad)
            var perpCos = Math.cos(needleRad + Math.PI / 2)
            var perpSin = Math.sin(needleRad + Math.PI / 2)

            // Shadow (offset slightly)
            ctx.save()
            ctx.shadowColor = "rgba(0,0,0,0.6)"
            ctx.shadowBlur = 15
            ctx.shadowOffsetX = 3
            ctx.shadowOffsetY = 3
            ctx.beginPath()
            ctx.moveTo(cx + needleLen * ncos, cy + needleLen * nsin)
            ctx.lineTo(cx + 5 * perpCos - needleTail * ncos, cy + 5 * perpSin - needleTail * nsin)
            ctx.lineTo(cx - 5 * perpCos - needleTail * ncos, cy - 5 * perpSin - needleTail * nsin)
            ctx.closePath()
            ctx.fillStyle = "rgba(0,0,0,0.5)"
            ctx.fill()
            ctx.restore()

            // Needle body with gradient feel
            // Dark base
            ctx.beginPath()
            ctx.moveTo(cx + needleLen * ncos, cy + needleLen * nsin)
            ctx.lineTo(cx + 4.5 * perpCos - needleTail * ncos, cy + 4.5 * perpSin - needleTail * nsin)
            ctx.lineTo(cx - 4.5 * perpCos - needleTail * ncos, cy - 4.5 * perpSin - needleTail * nsin)
            ctx.closePath()
            ctx.fillStyle = Qt.darker(needleColor, 1.4)
            ctx.fill()

            // Bright highlight side
            ctx.beginPath()
            ctx.moveTo(cx + needleLen * ncos, cy + needleLen * nsin)
            ctx.lineTo(cx + 2 * perpCos + needleLen * 0.1 * ncos, cy + 2 * perpSin + needleLen * 0.1 * nsin)
            ctx.lineTo(cx + 1 * perpCos - needleTail * 0.5 * ncos, cy + 1 * perpSin - needleTail * 0.5 * nsin)
            ctx.closePath()
            ctx.fillStyle = Qt.rgba(r || 1, g || 0.55, b || 0, 0.7)
            ctx.fill()

            // Needle tip glow
            ctx.beginPath()
            ctx.arc(cx + needleLen * 0.92 * ncos, cy + needleLen * 0.92 * nsin, 4, 0, Math.PI * 2)
            ctx.fillStyle = Qt.rgba(r || 1, g || 0.55, b || 0, 0.4)
            ctx.fill()

            // --- Center hub ---
            // Outer ring
            var hubGrad = ctx.createRadialGradient(cx - 3, cy - 3, 2, cx, cy, 18)
            hubGrad.addColorStop(0, "#666666")
            hubGrad.addColorStop(0.5, "#333333")
            hubGrad.addColorStop(1, "#1a1a1a")
            ctx.beginPath()
            ctx.arc(cx, cy, 16, 0, Math.PI * 2)
            ctx.fillStyle = hubGrad
            ctx.fill()
            ctx.strokeStyle = "#444444"
            ctx.lineWidth = 1.5
            ctx.stroke()

            // Inner colored dot
            ctx.beginPath()
            ctx.arc(cx, cy, 5, 0, Math.PI * 2)
            ctx.fillStyle = needleColor
            ctx.fill()

            // Hub highlight
            ctx.beginPath()
            ctx.arc(cx - 4, cy - 4, 3, 0, Math.PI * 2)
            ctx.fillStyle = "rgba(255,255,255,0.15)"
            ctx.fill()

            // --- Value readout ---
            ctx.save()
            ctx.fillStyle = "rgba(0,0,0,0.4)"
            var readoutW = 100
            var readoutH = 42
            var readoutY = cy + radius * 0.12
            roundRect(ctx, cx - readoutW/2, readoutY - readoutH/2, readoutW, readoutH, 6)
            ctx.fill()

            ctx.fillStyle = textColor
            ctx.font = "bold 36px 'Helvetica Neue', Helvetica, Arial, sans-serif"
            ctx.textAlign = "center"
            ctx.textBaseline = "middle"
            ctx.fillText(Math.round(displayValue).toString(), cx, readoutY)
            ctx.restore()

            // --- Unit label ---
            ctx.save()
            ctx.fillStyle = Qt.rgba(1, 1, 1, 0.35)
            ctx.font = "13px 'Helvetica Neue', Helvetica, Arial, sans-serif"
            ctx.textAlign = "center"
            ctx.textBaseline = "middle"
            ctx.fillText(unitLabel, cx, cy + radius * 0.32)
            ctx.restore()
        }

        function roundRect(ctx, x, y, w, h, r) {
            ctx.beginPath()
            ctx.moveTo(x + r, y)
            ctx.lineTo(x + w - r, y)
            ctx.quadraticCurveTo(x + w, y, x + w, y + r)
            ctx.lineTo(x + w, y + h - r)
            ctx.quadraticCurveTo(x + w, y + h, x + w - r, y + h)
            ctx.lineTo(x + r, y + h)
            ctx.quadraticCurveTo(x, y + h, x, y + h - r)
            ctx.lineTo(x, y + r)
            ctx.quadraticCurveTo(x, y, x + r, y)
            ctx.closePath()
        }
    }
}
