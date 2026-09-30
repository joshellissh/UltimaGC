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
// Every value here reads off the `sim` (CanBus) context property by
// default — see canbus.h's Q_PROPERTY list for what backs each key — except
// channels marked `source: "sysStats"`, which read off the `SystemStats`
// context property instead (board stats that aren't CAN signals at all; see
// systemstats.h). Channels flagged `unconfirmed: true` come from the MCE18
// (datasheet defaults, no unit on the bench yet) — everything else is
// SCal-verified.
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
    // Raw CAN frame monitor (tap the title to toggle) — shows every ID on the
    // bus with its bytes/rate/age, for confirming what's actually arriving
    // rather than what the decoders made of it. Polled only while shown.
    property bool showRaw: false
    property var rawFrames: []
    // Only the frames we actually decode (see GAUGE-CLUSTER.md's frame maps) —
    // listed in display order. An expected ID that hasn't arrived still gets a
    // row, flagged amber, so a silent ECU/MCE18 is visible instead of absent.
    readonly property var frameIds: ["0x600", "0x601", "0x604", "0x605", "0x608", "0x60A", "0x60E", "0x60F", "0x700", "0x702"]
    readonly property var frameNames: ({
        "0x600": "Syvecs F1: rpm, map", "0x601": "Syvecs F2: cruise",
        "0x604": "Syvecs F5: limp", "0x605": "Syvecs F6: ect, man/auto",
        "0x608": "Syvecs F9: eop", "0x60A": "Syvecs F11: cal, TC switch",
        "0x60E": "Syvecs F15: gear, vbat",
        "0x60F": "Syvecs F16: speed", "0x700": "MCE18: AIN0-3 (fuel)",
        "0x702": "MCE18: AIN8, DIN0-7"
    })
    Timer {
        interval: 500
        repeat: true
        running: root.showRaw && root.isOpen
        triggeredOnStart: true
        onTriggered: {
            var seen = {}
            var snap = sim.rawFrames()
            for (var i = 0; i < snap.length; ++i) seen[snap[i].id] = snap[i]
            var out = []
            for (var j = 0; j < root.frameIds.length; ++j) {
                var id = root.frameIds[j]
                out.push(seen[id] || { id: id, data: "--", hz: -1, ageMs: -1, count: 0 })
            }
            root.rawFrames = out
        }
    }

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
        { label: "Throttle (TPS1)", key: "tps", unit: "%", dec: 1, max: 100 },
        { label: "Boost", key: "boost", unit: "psi", dec: 1, max: 20 },
        { label: "Coolant Temp", key: "coolantTemp", unit: "°F", dec: 0, max: 260, critAt: 220 },
        { label: "Vehicle Speed", key: "speed", unit: "mph", dec: 0, max: 180 },
        { label: "Oil Pressure", key: "oilPressure", unit: "psi", dec: 0, max: 100 },
        { label: "Battery Volt", key: "vbat", unit: "V", dec: 2, max: 15.5 },
        { label: "Cruise State", key: "cruiseState", text: true },
        { label: "Limp Mode", key: "limpModeName", text: true },
        // Index into "PRN1234567" (see canbus.h's gear Q_PROPERTY comment),
        // not a magnitude — noBar below suppresses the bar for it.
        { label: "Gear", key: "gear", unit: "", dec: 0, min: 0, max: 9, noBar: true },
        { label: "Trans Mode", key: "transmissionAuto", bool: true, boolText: ["M", "A"] },
        // Raw values (0x60A slots 2/3) — calSelect is an enum 0-11, tcSwitch a
        // switch position; neither is a magnitude, so no bar.
        { label: "Cal Select", key: "calSelect", unit: "", dec: 0, min: 0, max: 11, noBar: true },
        { label: "TC Switch", key: "tcSwitch", unit: "", dec: 0, min: 0, max: 11, noBar: true },
        // MCE18 CAN expander — datasheet defaults, not wire-verified yet
        { label: "Fuel Level", key: "fuelLevel", unit: "%", dec: 0, max: 100, mult: 100, unconfirmed: true },
        { label: "Left Turn", key: "leftIndicator", bool: true, unconfirmed: true },
        { label: "Right Turn", key: "rightIndicator", bool: true, unconfirmed: true },
        { label: "Hazard", key: "hazard", bool: true, unconfirmed: true },
        { label: "Axle Lift", key: "axleLift", bool: true, unconfirmed: true },
        { label: "Low Beams", key: "lowBeams", bool: true, unconfirmed: true },
        { label: "High Beams", key: "highBeams", bool: true, unconfirmed: true },
        // Board stat, not a CAN signal — see systemstats.h. No critAt/warnAt:
        // this SoC's real thermal-throttle point hasn't been confirmed, so a
        // red/amber zone here would be a guess dressed up as a spec.
        { label: "CPU Temp", key: "cpuTempC", unit: "°C", dec: 0, max: 110, source: "sysStats" }
    ]

    // Which context property backs a channel — "sim" (CanBus) unless the
    // channel names a different one (currently only "sysStats").
    function dataFor(cfg) {
        return cfg.source === "sysStats" ? sysStats : sim
    }

    function fmtVal(cfg) {
        var v = root.dataFor(cfg)[cfg.key]
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
        var v = Number(root.dataFor(cfg)[cfg.key]) * (cfg.mult !== undefined ? cfg.mult : 1)
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
    // bar. Gear (noBar: true) is excluded for a different reason: it's a
    // position index ("PRN1234567"), not a magnitude, so a bar for it would
    // be meaningless even though its range is non-negative.
    function barVisible(cfg) {
        if (cfg.bool || cfg.text || cfg.noBar) return false
        if (cfg.min !== undefined && cfg.min < 0) return false
        return true
    }

    function tileState(cfg) {
        if (cfg.bool || cfg.text) return "normal"
        var v = Number(root.dataFor(cfg)[cfg.key])
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
        text: root.showRaw ? "CAN FRAMES  ▸ tap for values" : "DIAGNOSTICS  ▸ tap for CAN frames"
        MouseArea {
            anchors.fill: parent
            anchors.margins: -16
            onClicked: root.showRaw = !root.showRaw
        }
    }

    // y/rowSpacing trimmed from 78/16 to make room for the page indicator
    // below (see PageIndicator.qml) — the previous values put the grid's
    // bottom edge at y=706 in this 720px-tall screen, leaving only 14px,
    // not enough to fit dots without touching the last row.
    Grid {
        id: grid
        visible: !root.showRaw
        anchors.horizontalCenter: parent.horizontalCenter
        y: 66
        columns: 6
        rows: 4
        columnSpacing: 12
        rowSpacing: 10

        Repeater {
            model: root.channels
            delegate: Tile {
                width: 246
                height: 145
                cfg: modelData
            }
        }
    }

    Rectangle {
        visible: root.showRaw
        x: 40; y: 66; width: parent.width - 80; height: 616
        color: "#0a0a0a"; radius: 8
        border.width: 1; border.color: "#2a2c30"
        clip: true

        Row {
            id: rawHead
            x: 16; y: 8; height: 24
            spacing: 0
            Repeater {
                model: [["ID", 80], ["DATA", 250], ["HZ", 60], ["AGE", 100], ["COUNT", 90], ["FRAME", 230], ["CONVERTED VALUES", 640]]
                delegate: Text {
                    width: modelData[1]
                    font.family: bahnschriftFont.name; font.pixelSize: 13
                    color: "#8a8d93"; text: modelData[0]
                }
            }
        }
        Column {
            x: 16; y: 36
            Repeater {
                model: root.rawFrames
                delegate: Row {
                    height: 26
                    readonly property bool stale: modelData.ageMs < 0 || modelData.ageMs > 1500
                    readonly property color c: stale ? "#ff9500" : "white"
                    Text { width: 80; font.family: rangeFont.name; font.pixelSize: 17; color: parent.c; text: modelData.id }
                    Text { width: 250; font.family: rangeFont.name; font.pixelSize: 17; color: parent.c; text: modelData.data }
                    Text { width: 60; font.family: rangeFont.name; font.pixelSize: 17; color: parent.c; text: modelData.hz < 0 ? "--" : modelData.hz.toFixed(1) }
                    Text { width: 100; font.family: rangeFont.name; font.pixelSize: 17; color: parent.c; text: modelData.ageMs < 0 ? "NO DATA" : modelData.ageMs + " ms" }
                    Text { width: 90; font.family: rangeFont.name; font.pixelSize: 17; color: parent.c; text: modelData.count }
                    Text {
                        width: 230; font.family: bahnschriftFont.name; font.pixelSize: 15
                        elide: Text.ElideRight
                        color: "white"
                        text: root.frameNames[modelData.id]
                    }
                    Text {
                        width: 640; font.family: bahnschriftFont.name; font.pixelSize: 15
                        elide: Text.ElideRight
                        color: parent.c
                        text: modelData.values || "--"
                    }
                }
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
            // Text tiles (e.g. limpModeName's "SENSOR WARNING LEVEL") can be
            // wider than the tile — shrink to fit instead of overflowing.
            fontSizeMode: (tile.cfg && tile.cfg.text) ? Text.HorizontalFit : Text.FixedSize
            minimumPixelSize: 12
            width: (tile.cfg && tile.cfg.text) ? tile.width - 20 : implicitWidth
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
