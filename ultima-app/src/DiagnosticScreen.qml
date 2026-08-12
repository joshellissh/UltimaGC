import QtQuick 2.15

// Diagnostic screen — reached by swiping left from the main cluster. A
// single dense grid of every CAN2 channel the Auto Bionics mapping sheet
// documents, rather than the paged-category layout this started as: at
// 130x130 per tile, all 40 channels (pump1/2/3 combined into one tile) fit
// on one 10x4 grid with room to spare, so paging added navigation cost
// without buying anything. Swipe right to return to the main cluster.
//
// Every value here reads from CanBus's `diag` property (see the Q_PROPERTY
// comment in canbus.h) — none of these ~40 channels are decoded from real
// CAN2 yet, only simulated for dev-build layout review. Channels flagged
// `unconfirmed: true` are additionally unverified even as to their raw TCM
// channel names (ManualAuto_U12, ClutchAPress_U06, etc.) — unlike the rest,
// whose names are at least a known Syvecs signal, just not yet decoded on
// this build.
Item {
    id: root
    anchors.fill: parent
    visible: false
    z: 400  // above touchDot (100) and tripReset (200), below SetTimeScreen (500)

    property real _dragStartX: 0
    property bool _dragging: false

    function open() {
        visible = true
    }
    function close() {
        visible = false
    }

    FontLoader { id: bahnschriftFont; source: "qrc:/bahnschrift._semibold.ttf" }
    FontLoader { id: rangeFont; source: "qrc:/range.regular.ttf" }

    readonly property var channels: [
        // Air / Fuel Delivery
        { label: "Pedal Position", key: "pedalPct", unit: "%", dec: 0, max: 100 },
        { label: "Throttle Pos", key: "throttlePct", unit: "%", dec: 0, max: 100 },
        { label: "DBW Target", key: "dbwTargetPct", unit: "%", dec: 0, max: 100 },
        { label: "Charge Air Temp", key: "chargeAirTempC", unit: "°C", dec: 0, max: 80 },
        { label: "Wastegate Target", key: "wastegateTargetKpa", unit: "kPa", dec: 0, max: 250 },
        { label: "Engine Load", key: "loadPct", unit: "%", dec: 0, max: 100 },
        { label: "Closed Throttle", key: "throttleClosed", bool: true },
        { label: "Run Mode", key: "runMode", text: true },
        // Ignition & Knock
        { label: "Ignition Timing", key: "ignTimingDeg", unit: "°", dec: 1, max: 35 },
        { label: "Knock Cyl 1", key: "knock1", unit: "°", dec: 1, max: 5, warnAt: 1.0, critAt: 3.0 },
        { label: "Knock Cyl 2", key: "knock2", unit: "°", dec: 1, max: 5, warnAt: 1.0, critAt: 3.0 },
        { label: "Knock Cyl 3", key: "knock3", unit: "°", dec: 1, max: 5, warnAt: 1.0, critAt: 3.0 },
        { label: "Knock Cyl 4", key: "knock4", unit: "°", dec: 1, max: 5, warnAt: 1.0, critAt: 3.0 },
        { label: "Knock Cyl 5", key: "knock5", unit: "°", dec: 1, max: 5, warnAt: 1.0, critAt: 3.0 },
        { label: "Knock Cyl 6", key: "knock6", unit: "°", dec: 1, max: 5, warnAt: 1.0, critAt: 3.0 },
        { label: "Fuel Trim Cell", key: "fuelTrimCell", unit: "", dec: 2, max: 1.3 },
        // Fuel System
        { label: "Lambda Bank 1", key: "lambda1", unit: "λ", dec: 2, max: 1.3 },
        { label: "Lambda Bank 2", key: "lambda2", unit: "λ", dec: 2, max: 1.3 },
        { label: "Rail Pressure", key: "railPressureKpa", unit: "kPa", dec: 0, max: 600 },
        { label: "Injector Duty", key: "injectorDutyPct", unit: "%", dec: 0, max: 100 },
        { label: "Cons. Rate", key: "fuelConsRateCcMin", unit: "cc/m", dec: 0, max: 300 },
        { label: "Fuel Comp", key: "fuelCompPct", unit: "%", dec: 1, max: 10, min: -10 },
        { label: "Pump 1/2/3", keys: ["pump1", "pump2", "pump3"], bools: true },
        { label: "Vbat Comp", key: "vbatCompMs", unit: "ms", dec: 2, max: 2 },
        // Drivetrain & Torque
        { label: "Wheel Spin", key: "wheelSpinPct", unit: "%", dec: 1, max: 20, warnAt: 8 },
        { label: "TC Spin Error", key: "tcSpinErrPct", unit: "%", dec: 1, max: 10, min: -10 },
        { label: "TC Torque Cut", key: "tcTorqueCutPct", unit: "%", dec: 0, max: 30 },
        { label: "Launch RPM", key: "launchRpm", unit: "rpm", dec: 0, max: 7000 },
        { label: "Trq Output", key: "torqueOutputNm", unit: "Nm", dec: 0, max: 450 },
        { label: "Trq Demand", key: "torqueDemandNm", unit: "Nm", dec: 0, max: 450 },
        { label: "Cal / Map", key: "calSelect", unit: "", dec: 0, max: 11 },
        { label: "Pit Limiter", key: "pitLimiter", bool: true },
        // Trans — unconfirmed (raw TCM channel names not verified at all)
        { label: "Manual/Auto", key: "manualAuto", bool: true, boolText: ["M", "A"], unconfirmed: true },
        { label: "Paddle Down", key: "paddleDown", bool: true, unconfirmed: true },
        { label: "Sport+", key: "sportPlus", bool: true, unconfirmed: true },
        { label: "Clutch A Pres.", key: "clutchAPressureKpa", unit: "kPa", dec: 0, max: 600, unconfirmed: true },
        { label: "Clutch B Pres.", key: "clutchBPressureKpa", unit: "kPa", dec: 0, max: 600, unconfirmed: true },
        { label: "TCM Limp", key: "tcmLimp", bool: true, unconfirmed: true },
        { label: "TCM Logging", key: "tcmLogging", unit: "", dec: 0, max: 1, unconfirmed: true },
        { label: "Car DTC", key: "carDtc", unit: "", dec: 0, max: 5, unconfirmed: true }
    ]

    // sim.diagLive is false until the dev-build simulator has actually
    // ticked, and stays false forever on real hardware (decodeFrame()
    // doesn't touch diag yet — see canbus.h). Every accessor below gates on
    // it so the screen shows "no data" rather than diag's seeded resting
    // defaults as if they were live readings.
    function fmtVal(cfg) {
        if (!sim.diagLive) return "--"
        if (cfg.bools) {
            var parts = []
            for (var i = 0; i < cfg.keys.length; ++i)
                parts.push(sim.diag[cfg.keys[i]] ? "ON" : "OFF")
            return parts.join(" ")
        }
        var v = sim.diag[cfg.key]
        if (cfg.bool) {
            if (v === undefined) return "--"
            if (cfg.boolText) return v ? cfg.boolText[1] : cfg.boolText[0]
            return v ? "YES" : "NO"
        }
        if (cfg.text) return v === undefined ? "--" : v
        if (v === undefined) return "--"
        return Number(v).toFixed(cfg.dec !== undefined ? cfg.dec : 0)
    }

    function barFrac(cfg) {
        var v = Number(sim.diag[cfg.key])
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
    // channel (e.g. Fuel Comp, TC Spin Error: min < 0) — a resting 0 would
    // render as a half-full bar that reads as a real positive value. Those
    // channels get a number only; no bar.
    function barVisible(cfg) {
        if (!sim.diagLive) return false
        if (cfg.bool || cfg.bools || cfg.text) return false
        if (cfg.min !== undefined && cfg.min < 0) return false
        return true
    }

    function tileState(cfg) {
        if (!sim.diagLive) return "normal"
        if (cfg.bool || cfg.bools || cfg.text) return "normal"
        var v = Number(sim.diag[cfg.key])
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

    Grid {
        id: grid
        anchors.horizontalCenter: parent.horizontalCenter
        y: 78
        columns: 10
        rows: 4
        columnSpacing: 18
        rowSpacing: 18

        Repeater {
            model: root.channels
            delegate: Tile {
                width: 130
                height: 130
                cfg: modelData
            }
        }
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
            visible: tile.cfg && !tile.cfg.bool && !tile.cfg.bools && !tile.cfg.text
        }
    }
}
