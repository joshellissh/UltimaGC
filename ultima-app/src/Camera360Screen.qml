import QtQuick 2.15
import Ultima 1.0

// Full-screen camera overlay, opened/closed by tapping the 360 icon on the
// main dash (see main.qml's icon360) or automatically on reverse gear (see
// main.qml's reverseGear). Shows the 4 feeds from the mycam004m driver
// (cameraFeed1..cameraFeed4 context properties, see main.cpp) stitched into
// one top-down birds-eye view via SurroundView (surroundview.h) — a
// precomputed per-camera fisheye/ground-plane warp mesh + feather blend,
// the architecture test/avm-benchmark validated on real BeaglePlay hardware
// (see that project's docs/measurement-notes.md). Calibration is currently
// a placeholder rig (cameracalibration.cpp's defaultCalibration()) — seams
// and ground-plane scale will only be exactly right once real measured
// camera-mount data replaces it. CameraGridScreen.qml keeps the separate
// raw-quadrant debug layout this screen used to have.
Item {
    id: root
    anchors.fill: parent
    z: 600  // above SetTimeScreen (500); icon360 in main.qml sits higher still so it stays the tap target that closes this

    // Driven by open()/close() rather than a plain visible flag, so the
    // whole overlay (backdrop + grid + placeholder, via opacity inheritance)
    // cross-fades as one unit. visible tracks opacity so the fully-faded
    // closed state also stops swallowing touches to the dash underneath.
    opacity: 0
    visible: opacity > 0
    Behavior on opacity {
        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
    }

    // Order matters: SurroundView treats this array as
    // [front, rear, left, right] — see surroundview.h's class comment.
    readonly property var feeds: [cameraFeed1, cameraFeed2, cameraFeed3, cameraFeed4]

    readonly property bool anyStreaming: cameraFeed1.streaming || cameraFeed2.streaming
                                          || cameraFeed3.streaming || cameraFeed4.streaming
    readonly property bool allFailed: cameraFeed1.failed && cameraFeed2.failed
                                       && cameraFeed3.failed && cameraFeed4.failed

    // Bounded wait for the first live frame after opening, before falling
    // back to placeholder art — same reasoning as the single-feed version
    // this replaced, generalized to "none of the 4 have produced a frame
    // yet" rather than just one. Whole-screen fallback only, not per-
    // quadrant: this screen's job right now is proving the 4-stream path
    // works at all, not degrading gracefully feed-by-feed.
    property bool cameraTimedOut: false
    Timer {
        id: fallbackTimer
        interval: 2000
        onTriggered: cameraTimedOut = true
    }
    readonly property bool showPlaceholder: allFailed || cameraTimedOut
    onAnyStreamingChanged: {
        if (anyStreaming) {
            fallbackTimer.stop()
        } else {
            // Mid-stream loss (driver unplugged/unloaded), not just the
            // opening-hasn't-produced-a-frame-yet case fallbackTimer was
            // first armed for — restart it so all 4 feeds dying while the
            // overlay is open still degrades to the placeholder instead of
            // freezing on the last decoded frames forever.
            cameraTimedOut = false
            fallbackTimer.restart()
        }
    }

    function open() {
        opacity = 1
        cameraTimedOut = false
        fallbackTimer.restart()
        for (var i = 0; i < feeds.length; ++i)
            feeds[i].active = true
    }
    function close() {
        opacity = 0
        fallbackTimer.stop()
        calibrationScreen.close()
        for (var i = 0; i < feeds.length; ++i)
            feeds[i].active = false
    }

    // Swallow touches to the dash underneath while open.
    MouseArea {
        anchors.fill: parent
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    // The stitched birds-eye composite — see this file's top comment and
    // surroundview.h for the architecture. feeds order (cam1..cam4) maps to
    // front/rear/left/right — see surroundview.h's class comment for why
    // that mapping is currently a placeholder assumption.
    SurroundView {
        anchors.fill: parent
        feeds: root.feeds
        calibrationSource: calibrationStore
        visible: !showPlaceholder
    }

    // Calibration settings entry point — subtle by design (this is a tuning
    // tool, not primary UI). Drawn on a Canvas rather than a text glyph:
    // neither bundled font (bahnschrift, range) has a gear/settings glyph,
    // and this app has no guaranteed system fallback font on target (see
    // SetTimeScreen.qml's StepButton, which hit the identical problem for
    // its up/down triangles) — confirmed the hard way here too, a "−" step
    // button first drew as a missing-glyph box on real hardware. No PNG
    // asset exists yet in qml.qrc's set (icon_360.png etc.) either — a
    // simple 3-bar "menu" mark instead, swappable for a real icon later the
    // same way defaultCalibration()'s placeholder numbers are meant to be
    // swapped for real measured ones. Hidden once the panel itself is open
    // (it has its own CLOSE button) and while showPlaceholder (nothing live
    // to calibrate against).
    Rectangle {
        id: settingsIcon
        width: 44; height: 44
        radius: 8
        color: "transparent"
        opacity: settingsIconArea.pressed ? 0.35 : 0.55
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        visible: !showPlaceholder && !calibrationScreen.visible

        Canvas {
            anchors.centerIn: parent
            width: 26; height: 18
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = "white"
                var barH = 3
                var gap = (height - barH * 3) / 2
                ctx.fillRect(0, 0, width, barH)
                ctx.fillRect(0, barH + gap, width, barH)
                ctx.fillRect(0, (barH + gap) * 2, width, barH)
            }
        }

        MouseArea {
            id: settingsIconArea
            anchors.fill: parent
            anchors.margins: -12
            onClicked: calibrationScreen.open()
        }
    }

    // No anchors set here at all — CalibrationSettingsScreen.qml's root
    // already anchors its own top/bottom and manages its own x (the
    // slide-in position) internally; anchoring x here too would conflict
    // with that the same way anchors.fill did (see that file's header
    // comment).
    CalibrationSettingsScreen {
        id: calibrationScreen
    }
}
