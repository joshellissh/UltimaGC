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
// camera-mount data replaces it. simulated_cameras.png is kept as a
// whole-screen fallback for when no feed is up at all. CameraGridScreen.qml
// keeps the separate raw-quadrant debug layout this screen used to have.
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
        visible: !showPlaceholder
    }

    // Fallback art — shown only once every feed has genuinely failed, or
    // fallbackTimer has timed out waiting for a first frame from any of
    // them (e.g. the mycam004m driver isn't loaded / symlinks not set up).
    // Kept in qml.qrc for exactly this reason.
    Image {
        anchors.fill: parent
        source: "qrc:/simulated_cameras.png"
        visible: showPlaceholder
    }
}
