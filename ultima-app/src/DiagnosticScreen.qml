import QtQuick 2.15

// Diagnostic screen — reached by swiping left from the main cluster. A
// dense grid of every CAN2 signal this build actually decodes today, across
// both sources: the Syvecs S7+ ECU (verified against a current SCal
// Datastreams screenshot — see GAUGE-CLUSTER.md's "Verified CAN2 Frame Map")
// and the MCE18 CAN expander (still datasheet-default, not wire-confirmed —
// see GAUGE-CLUSTER.md's MCE18 section). Swipe right to return to the main
// cluster.
//
// Most of these channels also drive something on the main dash, but only as
// a needle position or a lit/unlit icon — this screen exists to show the
// actual decoded number or enum behind that, which is what you want when
// verifying a signal is wired correctly rather than just glancing at it.
// Every value here reads directly off the `sim` (CanBus) context property —
// see canbus.h's Q_PROPERTY list for what backs each key. Channels flagged
// `unconfirmed: true` come from the MCE18 (datasheet defaults, no unit on
// the bench yet) or rely on an assumed-not-confirmed polarity
// (`transmissionAuto`) — everything else is SCal-verified.
Item {
    id: root
    y: 0
    width: parent.width
    height: parent.height
    z: 400  // above touchDot (100) and tripReset (200), below SetTimeScreen (500)

    // Slides in from offscreen right on open(), back out on close() — matches
    // the swipe gesture that drives it (swipe left reveals this screen
    // sliding in from the right; swipe right sends it back out that way).
    // Starts at parent.width (fully offscreen) with no animation, since
    // Behavior doesn't apply to a property's initial value during
    // construction — only open()/close()'s later imperative assignments
    // animate. Offscreen x also means the MouseArea below never overlaps the
    // window while closed, so it doesn't need a separate visible flag to
    // stop swallowing touches to the dash underneath.
    x: parent.width
    Behavior on x {
        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
    }

    property real _dragStartX: 0
    property bool _dragging: false

    // Tracks open/closing state for things outside this screen (e.g. the
    // 360 icon in main.qml) that need to hide while this is in front —
    // true for the whole open()->close() cycle, not just the resting-open
    // state, so it hides as soon as the swipe starts rather than only once
    // it lands.
    readonly property bool isOpen: x !== parent.width

    function open() {
        x = 0
    }
    function close() {
        x = parent.width
    }

    FontLoader { id: bahnschriftFont; source: "qrc:/bahnschrift._semibold.ttf" }
    FontLoader { id: rangeFont; source: "qrc:/range.regular.ttf" }

    readonly property var channels: [
        // ECU (Syvecs S7+) — verified frame map, see GAUGE-CLUSTER.md
        { label: "RPM", key: "rpm", unit: "", dec: 0, max: 7500 },
        { label: "Boost", key: "boost", unit: "psi", dec: 1, max: 30 },
        { label: "Coolant Temp", key: "coolantTemp", unit: "°F", dec: 0, max: 260, critAt: 220 },
        { label: "Vehicle Speed", key: "speed", unit: "mph", dec: 0, max: 180 },
        { label: "Oil Pressure", key: "oilPressure", unit: "psi", dec: 0, max: 100 },
        { label: "Battery Volt", key: "vbat", unit: "V", dec: 2, max: 15.5 },
        { label: "Cruise State", key: "cruiseState", text: true },
        { label: "Limp Mode", key: "limpMode", text: true },
        { label: "Gear", key: "gear", unit: "", dec: 0, min: -2, max: 8 },
        // Auto/Manual polarity is assumed, not confirmed — see canbus.h
        { label: "Trans Mode", key: "transmissionAuto", bool: true, boolText: ["M", "A"], unconfirmed: true },
        // MCE18 CAN expander — datasheet defaults, not wire-verified yet
        { label: "Fuel Level", key: "fuelLevel", unit: "%", dec: 0, max: 100, mult: 100, unconfirmed: true },
        { label: "Left Turn", key: "leftIndicator", bool: true, unconfirmed: true },
        { label: "Right Turn", key: "rightIndicator", bool: true, unconfirmed: true },
        { label: "Axle Lift", key: "axleLift", bool: true, unconfirmed: true },
        { label: "Low Beams", key: "lowBeams", bool: true, unconfirmed: true },
        { label: "High Beams", key: "highBeams", bool: true, unconfirmed: true }
    ]

    function fmtVal(cfg) {
        var v = sim[cfg.key]
        if (cfg.bool) {
            if (v === undefined) return "--"
            if (cfg.boolText) return v ? cfg.boolText[1] : cfg.boolText[0]
            return v ? "YES" : "NO"
        }
        if (cfg.text) return v === undefined ? "--" : v
        if (v === undefined) return "--"
        return (Number(v) * (cfg.mult !== undefined ? cfg.mult : 1)).toFixed(cfg.dec !== undefined ? cfg.dec : 0)
    }

    function barFrac(cfg) {
        var v = Number(sim[cfg.key]) * (cfg.mult !== undefined ? cfg.mult : 1)
        if (isNaN(v)) return 0
        var lo = cfg.min !== undefined ? cfg.min : 0
        var hi = cfg.max !== undefined ? cfg.max : 100
        return Math.max(0, Math.min(1, (v - lo) / (hi - lo)))
    }

    // Bar-zone geometry: the vertical bar is drawn top-down as max..min, so
    // "distance from the top" is what places a threshold value on it.
    function fracFromTop(cfg, v) {
        var lo = cfg.min !== undefined ? cfg.min : 0
        var hi = cfg.max !== undefined ? cfg.max : 100
        return Math.max(0, Math.min(1, (hi - v) / (hi - lo)))
    }
    // Height (as a fraction of the track) of the red critical zone, anchored
    // at the top of the bar — empty if the channel has no critAt.
    function critZoneFrac(cfg) {
        return cfg.critAt !== undefined ? root.fracFromTop(cfg, cfg.critAt) : 0
    }
    // Height of the amber warn band sitting directly below the crit zone
    // (or below the top of the bar, if there's no crit zone).
    function warnZoneFrac(cfg) {
        if (cfg.warnAt === undefined) return 0
        return Math.max(0, root.fracFromTop(cfg, cfg.warnAt) - root.critZoneFrac(cfg))
    }
    // Whatever's left below crit+warn — the safe zone.
    function safeZoneFrac(cfg) {
        return Math.max(0, 1 - root.critZoneFrac(cfg) - root.warnZoneFrac(cfg))
    }

    // A fixed-zero-origin bar misrepresents a signed/centered-on-zero
    // channel (min < 0) — a resting 0 would render as a half-full bar that
    // reads as a real positive value. Those channels get a number only; no
    // bar. Gear is the one channel here that fits that shape (-2..8).
    function barVisible(cfg) {
        if (cfg.bool || cfg.text) return false
        if (cfg.min !== undefined && cfg.min < 0) return false
        return true
    }

    function tileState(cfg) {
        if (cfg.bool || cfg.text) return "normal"
        var v = Number(sim[cfg.key])
        if (cfg.critAt !== undefined && v >= cfg.critAt) return "crit"
        if (cfg.warnAt !== undefined && v >= cfg.warnAt) return "warn"
        return "normal"
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    // Swallow touches to the dash underneath while open; a swipe right
    // (past the drag threshold) returns to the main cluster. Plain
    // MouseArea press/position deltas, matching the primitive main.qml
    // already uses elsewhere (tripReset, touchDot) rather than pulling in
    // QtQuick.Controls' SwipeView for the first time on a Qt5/linuxfb
    // target this app hasn't proven it on.
    MouseArea {
        anchors.fill: parent
        onPressed: { root._dragStartX = mouse.x; root._dragging = true }
        // Fires as soon as the threshold is crossed mid-drag rather than
        // waiting for onReleased — see the matching comment on main.qml's
        // swipe MouseArea for why release isn't a reliable trigger here.
        onPositionChanged: {
            if (!root._dragging) return
            if (mouse.x - root._dragStartX > 120) {
                root._dragging = false
                root.close()
            }
        }
    }

    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 24
        font.family: bahnschriftFont.name
        font.pixelSize: 22
        color: "white"
        text: "DIAGNOSTICS"
    }

    // y/rowSpacing trimmed from 78/16 to make room for the page indicator
    // below (see PageIndicator.qml) — the previous values put the grid's
    // bottom edge at y=706 in this 720px-tall screen, leaving only 14px,
    // not enough to fit dots without touching the last row.
    Grid {
        id: grid
        anchors.horizontalCenter: parent.horizontalCenter
        y: 66
        columns: 4
        rows: 4
        columnSpacing: 18
        rowSpacing: 10

        Repeater {
            model: root.channels
            delegate: Tile {
                width: 260
                height: 145
                cfg: modelData
            }
        }
    }

    // Page indicator — this screen is the "right" page (see
    // PageIndicator.qml comment).
    PageIndicator {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 696
        currentIndex: 2
    }

    component Tile: Rectangle {
        id: tile
        property var cfg
        readonly property string state: cfg ? root.tileState(cfg) : "normal"

        radius: 8
        color: "#0a0a0a"
        border.width: 1
        border.color: cfg && cfg.unconfirmed ? "#33363b" : "#2a2c30"

        // Header: centered label over a full-width rule, matching the
        // reference layout — label/rule stay fixed height so the value and
        // bar below always start from the same y regardless of label wrap.
        Column {
            id: header
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 8
            spacing: 5

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                font.family: bahnschriftFont.name
                font.pixelSize: 11
                color: "white"
                elide: Text.ElideRight
                maximumLineCount: 1
                text: (tile.cfg ? tile.cfg.label.toUpperCase() : "") + (tile.cfg && tile.cfg.unconfirmed ? " *" : "")
            }
            Rectangle { width: parent.width; height: 1; color: "#2a2c30" }
        }

        // Vertical bar: drawn top-down as max..min. The dim crit/warn/safe
        // segments are a static map of the channel's thresholds (always
        // visible, regardless of current value — like a redline painted on
        // a gauge face); the bright overlay anchored to the bottom is the
        // actual current-value fill, same fraction as the old horizontal
        // bar used (barFrac).
        Item {
            id: barTrack
            anchors.top: header.bottom
            anchors.topMargin: 10
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 8
            width: 14
            visible: tile.cfg && root.barVisible(tile.cfg)

            Rectangle {
                y: 0
                width: parent.width
                height: parent.height * (tile.cfg ? root.critZoneFrac(tile.cfg) : 0)
                color: "#3a1414"
            }
            Rectangle {
                y: parent.height * (tile.cfg ? root.critZoneFrac(tile.cfg) : 0)
                width: parent.width
                height: parent.height * (tile.cfg ? root.warnZoneFrac(tile.cfg) : 0)
                color: "#3a2c14"
            }
            Rectangle {
                y: parent.height * (tile.cfg ? (root.critZoneFrac(tile.cfg) + root.warnZoneFrac(tile.cfg)) : 0)
                width: parent.width
                height: parent.height * (tile.cfg ? root.safeZoneFrac(tile.cfg) : 1)
                color: (tile.cfg && (tile.cfg.warnAt !== undefined || tile.cfg.critAt !== undefined)) ? "#123320" : "#202225"
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: parent.height * (tile.cfg ? root.barFrac(tile.cfg) : 0)
                color: tile.state === "crit" ? "#ff3b30" : (tile.state === "warn" ? "#ff9500" : "#34c759")
            }
        }

        Text {
            id: valText
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: barTrack.verticalCenter
            font.family: rangeFont.name
            font.pixelSize: 28
            color: tile.state === "crit" ? "#ff3b30" : (tile.state === "warn" ? "#ff9500" : "white")
            text: tile.cfg ? root.fmtVal(tile.cfg) : ""
        }
        Text {
            anchors.left: valText.right
            anchors.leftMargin: 3
            anchors.baseline: valText.baseline
            font.family: bahnschriftFont.name
            font.pixelSize: 12
            color: "white"
            text: (tile.cfg && tile.cfg.unit) ? tile.cfg.unit : ""
            visible: tile.cfg && !tile.cfg.bool && !tile.cfg.text
        }
    }
}
