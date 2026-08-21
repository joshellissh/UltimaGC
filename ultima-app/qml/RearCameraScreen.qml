import QtQuick 2.15
import Ultima 1.0

// Full-screen single-camera overlay, opened/closed automatically on reverse
// gear (see main.qml's reverseGear) — a real backup camera shows the rear
// camera alone, not a stitched surround view, so reverse gear drives this
// screen instead of Camera360Screen. Shows cameraFeed2 raw (the "rear" feed
// — see Camera360Screen.qml's [front, rear, left, right] feeds-order comment
// for why index 1, cameraFeed2, is rear). Camera360Screen itself is
// unchanged and still reachable manually by tapping car_lights_off for the
// full 4-camera surround view.
Item {
    id: root
    anchors.fill: parent
    z: 600  // same stacking spot Camera360Screen used for this role

    // Driven by open()/close() rather than a plain visible flag, so the
    // whole overlay cross-fades as one unit — same reasoning as
    // Camera360Screen's opacity/visible pair.
    opacity: 0
    visible: opacity > 0
    Behavior on opacity {
        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
    }

    // Bounded wait for the first live frame after opening, before falling
    // back to a black screen — same reasoning as Camera360Screen's fallback
    // timer, scoped here to the single rear feed instead of all 4.
    readonly property bool anyStreaming: cameraFeed2.streaming
    property bool cameraTimedOut: false
    Timer {
        id: fallbackTimer
        interval: 2000
        onTriggered: cameraTimedOut = true
    }
    readonly property bool showPlaceholder: cameraFeed2.failed || cameraTimedOut
    onAnyStreamingChanged: {
        if (anyStreaming) {
            fallbackTimer.stop()
        } else {
            cameraTimedOut = false
            fallbackTimer.restart()
        }
    }

    // cameraFeed2.active itself is driven centrally by main.qml (see its
    // "Single owner of every CameraFeed's active state" comment) — visible
    // (derived from opacity above) is what main.qml ORs into that, so
    // open()/close() only need to drive this screen's own fade state.
    function open() {
        opacity = 1
        cameraTimedOut = false
        if (anyStreaming) {
            fallbackTimer.stop()
        } else {
            fallbackTimer.restart()
        }
    }
    function close() {
        opacity = 0
        fallbackTimer.stop()
    }

    // Swallow touches to the dash underneath while open.
    MouseArea {
        anchors.fill: parent
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    // Raw rear feed, pillarboxed to whatever aspect ratio CameraFeed
    // actually negotiated — same fit math as CameraGridScreen's per-
    // quadrant CameraView, just sized to the full screen instead of a
    // quadrant.
    CameraView {
        feed: cameraFeed2
        anchors.centerIn: parent
        height: parent.height
        width: cameraFeed2.frameHeight > 0
               ? Math.round(parent.height * (cameraFeed2.frameWidth / cameraFeed2.frameHeight))
               : Math.round(parent.height * 16 / 9)
        visible: !root.showPlaceholder
    }
}
