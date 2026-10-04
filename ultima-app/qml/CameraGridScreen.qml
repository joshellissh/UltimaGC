import QtQuick 2.15
import Ultima 1.0

// Camera grid screen — reached by swiping right from the main cluster (mirror
// image of DiagnosticScreen, which owns swipe-left). Shows the 4 raw camera
// feeds (cameraFeed1..cameraFeed4 context properties, see main.cpp) as a
// plain cross layout (front top-center, rear under it, left/right
// at the sides), one tile per physical camera, no
// stitching — same content Camera360Screen shows on tap/reverse-gear, but
// reached as a persistent swipeable screen instead of a tap-triggered
// overlay. Deliberately a separate screen/file rather than sharing
// Camera360Screen: that screen's opacity-fade + reverse-gear auto-open
// behavior stays untouched, and this one's slide mechanics match
// DiagnosticScreen's swipe convention instead.
//
// Decoding + converting 4x 1920x1080 UYVY frames every tick is real CPU/GPU
// work — feeds[i].active only goes true while this screen
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

    // visible tracks the slide the same way RearCameraScreen/Camera360Screen
    // track opacity: fully slid away = genuinely invisible (still visible
    // through the whole 250ms slide, since x has already left/not yet hit
    // -parent.width). Without this the closed screen sits at x=-width with
    // visible still true, and CameraView's isVisible()-gated
    // frameReady->update() (see cameraview.cpp's setFeed()) can't tell it's
    // off-screen — all 4 tiles kept re-rendering their FBOs at frame rate
    // whenever any feed happened to stream (e.g. a mirror overlay open with
    // the grid closed): measured as 4 extra ~25/s renderers on hardware.
    visible: isOpen

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

    // Cross layout, one tile per camera (feeds order is [front, rear, left,
    // right] — see Camera360Screen.qml): front top-center with rear directly
    // under it, left/right flush to the screen edges and vertically centered.
    // Tiles are 16:9; the center column is 2 tiles + gap tall, clear of the
    // title above and the page indicator below. CameraView is pillarboxed to
    // whatever aspect ratio CameraFeed actually negotiated.
    Item {
        id: layout
        anchors.fill: parent
        visible: !root.showPlaceholder

        readonly property real tileH: 296
        readonly property real tileW: Math.round(tileH * 16 / 9)
        readonly property real gap: 8
        readonly property real stackTop: 60
        readonly property real centerX: (width - tileW) / 2

        readonly property var tiles: [
            { x: centerX,           y: stackTop },
            { x: centerX,           y: stackTop + tileH + gap },
            { x: 0,                 y: (height - tileH) / 2 },
            { x: width - tileW,     y: (height - tileH) / 2 }
        ]

        Repeater {
            model: layout.tiles

            Item {
                id: tile
                x: modelData.x
                y: modelData.y
                width: layout.tileW
                height: layout.tileH
                property var feed: root.feeds[index]

                CameraView {
                    feed: tile.feed
                    anchors.centerIn: parent
                    height: parent.height
                    width: tile.feed.frameHeight > 0
                           ? Math.round(parent.height * (tile.feed.frameWidth / tile.feed.frameHeight))
                           : Math.round(parent.height * 16 / 9)
                    visible: !tile.feed.failed
                }

                Text {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 8
                    color: "white"
                    style: Text.Outline
                    styleColor: "black"
                    font.pixelSize: 18
                    text: tile.feed.streaming ? "" : tile.feed.failed ? "FAILED" : "NO SIGNAL"
                }
            }
        }
    }

    // Page indicator — this screen is the "left" page (see PageIndicator.qml
    // comment). Declared after the layout so it paints on top of the camera
    // feeds; its dots carry their own dark border for contrast against
    // whatever's under them, same reasoning as the outlined "CAM N" labels
    // above.
    PageIndicator {
        anchors.horizontalCenter: parent.horizontalCenter
        y: 696
        currentIndex: 0
    }
}
