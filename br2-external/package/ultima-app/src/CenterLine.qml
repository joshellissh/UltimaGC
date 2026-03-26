import QtQuick 2.15

Canvas {
    id: centerDecor

    onPaint: {
        var ctx = getContext("2d")
        var w = width
        var h = height
        ctx.clearRect(0, 0, w, h)

        var vpX = w / 2
        var vpY = h * 0.08

        // Radial glow behind vanishing point
        var glow = ctx.createRadialGradient(vpX, vpY + h * 0.2, 0, vpX, vpY + h * 0.2, h * 0.5)
        glow.addColorStop(0, "rgba(0, 206, 209, 0.06)")
        glow.addColorStop(1, "rgba(0, 206, 209, 0)")
        ctx.fillStyle = glow
        ctx.fillRect(0, 0, w, h)

        // Perspective grid lines
        var lineCount = 7
        for (var i = 0; i < lineCount; i++) {
            var frac = (i + 1) / (lineCount + 1)
            var y = vpY + frac * (h * 0.75)
            var spread = w * 0.48 * frac * frac  // quadratic spread for more realism

            var alpha = 0.04 + 0.10 * (1 - frac)

            // Main line
            ctx.beginPath()
            ctx.moveTo(vpX - spread, y)
            ctx.lineTo(vpX + spread, y)
            ctx.strokeStyle = Qt.rgba(0, 0.808, 0.82, alpha)
            ctx.lineWidth = 1
            ctx.stroke()

            // Glow underneath
            ctx.beginPath()
            ctx.moveTo(vpX - spread * 0.8, y)
            ctx.lineTo(vpX + spread * 0.8, y)
            ctx.strokeStyle = Qt.rgba(0, 0.808, 0.82, alpha * 0.4)
            ctx.lineWidth = 4
            ctx.stroke()
        }

        // Converging side lines (subtle)
        var bottomY = vpY + h * 0.72
        var sideSpread = w * 0.44
        for (var s = -1; s <= 1; s += 2) {
            ctx.beginPath()
            ctx.moveTo(vpX + s * 8, vpY + h * 0.1)
            ctx.lineTo(vpX + s * sideSpread, bottomY)
            ctx.strokeStyle = Qt.rgba(0, 0.808, 0.82, 0.04)
            ctx.lineWidth = 0.5
            ctx.stroke()
        }

        // Center vertical (very faint)
        ctx.beginPath()
        ctx.moveTo(vpX, vpY + 20)
        ctx.lineTo(vpX, bottomY)
        var vertGrad = ctx.createLinearGradient(vpX, vpY + 20, vpX, bottomY)
        vertGrad.addColorStop(0, "rgba(0, 206, 209, 0.08)")
        vertGrad.addColorStop(0.5, "rgba(0, 206, 209, 0.03)")
        vertGrad.addColorStop(1, "rgba(0, 206, 209, 0)")
        ctx.strokeStyle = vertGrad
        ctx.lineWidth = 1
        ctx.stroke()

        // Small diamond at vanishing point
        var dSize = 4
        var dY = vpY + h * 0.15
        ctx.beginPath()
        ctx.moveTo(vpX, dY - dSize)
        ctx.lineTo(vpX + dSize, dY)
        ctx.lineTo(vpX, dY + dSize)
        ctx.lineTo(vpX - dSize, dY)
        ctx.closePath()
        ctx.fillStyle = Qt.rgba(0, 0.808, 0.82, 0.2)
        ctx.fill()
    }
}
