import QtQuick 2.15
import Ultima 1.0

// Full-screen camera overlay, opened/closed by tapping the 360 icon on the
// main dash (see main.qml's icon360) or automatically on reverse gear (see
// main.qml's reverseGear). Shows the 4 raw feeds from the mycam004m driver
// (cameraFeed1..cameraFeed4 context properties, see main.cpp) as a plain
// 2x2 grid — one quadrant per physical camera, no stitching. Birds-eye/360
// compositing of these 4 feeds into one view is deferred to later work;
// this is the interim step of proving the 4-stream device-driver path
// (fake backend today, see ~/code/mycam004m/docs/ultima-app-integration.md)
// actually delivers frames to the screen. simulated_cameras.png is kept as
// a whole-screen fallback for when no feed is up at all.
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

    // Raster order (top-left, top-right, bottom-left, bottom-right) matches
    // cam1..cam4 — see gen_fake_frames.py's per-camera reference images and
    // the integration doc's verification step 3: a swapped cam2/cam3 should
    // be immediately visible by which quadrant shows which color/label,
    // not something that needs pixel inspection to catch.
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

    // 2x2 grid, one quadrant per camera. Each quadrant pillarboxes its feed
    // to whatever aspect ratio CameraFeed actually negotiated (mycam004m is
    // a fixed 1920x1080, but this stays robust rather than hardcoding
    // 16:9 — same reasoning the single-feed version used for a PAL source).
    Grid {
        anchors.fill: parent
        columns: 2
        rows: 2
        visible: !showPlaceholder

        Repeater {
            model: 4

            Item {
                id: quadrant
                width: root.width / 2
                height: root.height / 2
                property var feed: root.feeds[index]

                CameraView {
                    feed: quadrant.feed
                    anchors.centerIn: parent
                    height: parent.height
                    width: quadrant.feed.frameHeight > 0
                           ? Math.round(parent.height * (quadrant.feed.frameWidth / quadrant.feed.frameHeight))
                           : Math.round(parent.height * 16 / 9)
                    visible: !quadrant.feed.failed
                }

                // Per-quadrant label — which physical camera this is, and
                // whether it's actually delivering frames yet. Cheap and
                // exactly what's needed to confirm on real hardware that
                // cam1..cam4 land in the right quadrants and are streaming,
                // without needing pixel inspection.
                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 8
                    color: "white"
                    style: Text.Outline
                    styleColor: "black"
                    font.pixelSize: 18
                    text: "CAM " + (index + 1) + (quadrant.feed.streaming ? "" : quadrant.feed.failed ? "  FAILED" : "  NO SIGNAL")
                }
            }
        }
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
