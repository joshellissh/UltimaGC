import QtQuick 2.15
import Ultima 1.0

// Camera grid screen — reached by swiping right from the main cluster (mirror
// image of DiagnosticScreen, which owns swipe-left). Shows the 4 raw feeds
// from the mycam004m driver (cameraFeed1..cameraFeed4 context properties, see
// main.cpp) as a plain 2x2 grid, one quadrant per physical camera, no
// stitching — same content Camera360Screen shows on tap/reverse-gear, but
// reached as a persistent swipeable screen instead of a tap-triggered
// overlay. Deliberately a separate screen/file rather than sharing
// Camera360Screen: that screen's opacity-fade + reverse-gear auto-open
// behavior stays untouched, and this one's slide mechanics match
// DiagnosticScreen's swipe convention instead.
//
// Decoding + converting 4x 1920x1080 YUYV frames every tick is real CPU/GPU
// work (see ~/code/mycam004m/docs/ultima-app-integration.md's GUI-thread
// conversion cost note) — feeds[i].active only goes true while this screen
// is actually open, same lazy-open contract CameraFeed already uses for
// Camera360Screen, so a drive that never swipes here costs nothing.
Item {
    id: root
    y: 0
    width: parent.width
    height: parent.height
    z: 400  // above touchDot (100) and tripReset (200), below SetTimeScreen (500)

    // Slides in from offscreen left on open(), back out on close() — swipe
    // right reveals this screen sliding in from the left; swipe left sends
    // it back out that way. Mirror of DiagnosticScreen's parent.width/x:0,
    // using -parent.width since this is the opposite swipe direction. Starts
    // fully offscreen with no animation for the same reason DiagnosticScreen
    // does: Behavior doesn't apply to a property's initial value during
    // construction.
    x: -parent.width
    Behavior on x {
        NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
    }

    property real _dragStartX: 0
    property bool _dragging: false

    readonly property bool isOpen: x !== -parent.width

    readonly property var feeds: [cameraFeed1, cameraFeed2, cameraFeed3, cameraFeed4]

    readonly property bool anyStreaming: cameraFeed1.streaming || cameraFeed2.streaming
                                          || cameraFeed3.streaming || cameraFeed4.streaming
    readonly property bool allFailed: cameraFeed1.failed && cameraFeed2.failed
                                       && cameraFeed3.failed && cameraFeed4.failed

    // Bounded wait for the first live frame after opening, before falling
    // back to placeholder art — see Camera360Screen's identical logic.
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
            cameraTimedOut = false
            fallbackTimer.restart()
        }
    }

    // feeds[i].active itself is driven centrally by main.qml (see its
    // "Single owner of every CameraFeed's active state" comment) — isOpen
    // (derived from x below) is what main.qml ORs into that, so open()/
    // close() only need to drive this screen's own slide state.
    function open() {
        x = 0
        cameraTimedOut = false
        // See Camera360Screen.qml's open() for why this checks current
        // state instead of unconditionally restarting: these two screens
        // share the same feeds[], so reopening one while the other left
        // them streaming means anyStreaming is already true and won't
        // change value, and a blind restart would blank to the placeholder
        // 2s later over a perfectly working stream.
        if (anyStreaming) {
            fallbackTimer.stop()
        } else {
            fallbackTimer.restart()
        }
    }
    function close() {
        x = -parent.width
        fallbackTimer.stop()
    }

    FontLoader { id: bahnschriftFont; source: "qrc:/bahnschrift._semibold.ttf" }

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    // Swallow touches to the dash underneath while open; a swipe left (past
    // the drag threshold) returns to the main cluster — mirror of
    // DiagnosticScreen's swipe-right-to-close MouseArea.
    MouseArea {
        anchors.fill: parent
        onPressed: { root._dragStartX = mouse.x; root._dragging = true }
        onPositionChanged: {
            if (!root._dragging) return
            if (mouse.x - root._dragStartX < -120) {
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
        text: "CAMERAS"
    }

    // 2x2 grid, one quadrant per camera — identical layout to
    // Camera360Screen's, pillarboxed to whatever aspect ratio CameraFeed
    // actually negotiated.
    Grid {
        anchors.fill: parent
        columns: 2
        rows: 2
        visible: !root.showPlaceholder

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

    // Page indicator — this screen is the "left" page (see PageIndicator.qml
    // comment). Declared after the Grid so it paints on top of the camera
    // feeds; its dots carry their own dark border for contrast against
    // whatever's under them, same reasoning as the outlined "CAM N" labels
    // above.
    PageIndicator {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 696
        currentIndex: 0
    }
}
