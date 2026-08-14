import QtQuick 2.15
import Ultima 1.0

// Full-screen 360-degree camera view overlay, opened/closed by tapping the
// 360 icon on the main dash (see main.qml's icon360) or automatically on
// reverse gear (see main.qml's reverseGear). Shows the live feed from
// CameraFeed (cameraFeed context property, see main.cpp) — a USB UVC
// capture card wired to a single rear/composite camera. simulated_cameras.png
// is kept as a fallback (see showPlaceholder below), not removed from
// qml.qrc.
Item {
    id: root
    anchors.fill: parent
    z: 600  // above SetTimeScreen (500); icon360 in main.qml sits higher still so it stays the tap target that closes this

    // Driven by open()/close() rather than a plain visible flag, so the
    // whole overlay (backdrop + both image layers, via opacity inheritance)
    // cross-fades as one unit. visible tracks opacity so the fully-faded
    // closed state also stops swallowing touches to the dash underneath.
    opacity: 0
    visible: opacity > 0
    Behavior on opacity {
        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
    }

    // Bounded wait for the first live frame after opening, before falling
    // back to the placeholder art. streaming only goes true once a frame
    // has actually arrived — tens to hundreds of ms after cameraFeed.active,
    // longer if the grabber is still enumerating — so a plain "!streaming"
    // fallback would flash the fake 360 composite on every single open
    // before cutting to video, worst of all on a reverse-gear auto-open
    // where the real feed is wanted immediately. Until either streaming
    // arrives or this timer fires, showPlaceholder is false and so is
    // cameraView's visible: just the black backdrop below, no flash.
    property bool cameraTimedOut: false
    Timer {
        id: fallbackTimer
        interval: 2000
        onTriggered: cameraTimedOut = true
    }
    readonly property bool showPlaceholder: cameraFeed.failed || cameraTimedOut
    Connections {
        target: cameraFeed
        function onStreamingChanged() {
            if (cameraFeed.streaming) {
                fallbackTimer.stop()
            } else if (cameraFeed.active) {
                // Mid-stream loss (grabber unplugged, bus error), not the
                // opening-hasn't-produced-a-frame-yet case fallbackTimer was
                // first armed for — restart it so a disconnect while the
                // overlay is open still degrades to the placeholder instead
                // of freezing on CameraView's last decoded frame forever.
                cameraTimedOut = false
                fallbackTimer.restart()
            }
        }
    }

    function open() {
        opacity = 1
        cameraTimedOut = false
        fallbackTimer.restart()
        cameraFeed.active = true
    }
    function close() {
        opacity = 0
        fallbackTimer.stop()
        cameraFeed.active = false
    }

    // Swallow touches to the dash underneath while open.
    MouseArea {
        anchors.fill: parent
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: 0.82
    }

    // Live feed, pillarboxed to whatever aspect ratio CameraFeed actually
    // negotiated (VIDIOC_S_FMT is a negotiation, not a command — see
    // camerafeed.cpp) rather than a hardcoded 4:3, so a PAL 720x576 source
    // still renders correctly. Hidden (not just covered) while
    // showPlaceholder is true, and during the black-backdrop-only window
    // described above.
    CameraView {
        id: cameraView
        feed: cameraFeed
        anchors.centerIn: parent
        height: parent.height
        width: cameraFeed.frameHeight > 0
               ? Math.round(parent.height * (cameraFeed.frameWidth / cameraFeed.frameHeight))
               : Math.round(parent.height * 4 / 3)
        visible: !showPlaceholder
    }

    // Fallback art — shown only once cameraFeed has genuinely failed, or
    // fallbackTimer has timed out waiting for a first frame (e.g. grabber
    // unplugged). Kept in qml.qrc for exactly this reason.
    Image {
        anchors.fill: parent
        source: "qrc:/simulated_cameras.png"
        visible: showPlaceholder
    }
}
