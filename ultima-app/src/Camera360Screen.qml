import QtQuick 2.15

// Full-screen 360-degree camera view overlay, opened/closed by tapping the
// 360 icon on the main dash (see main.qml's icon360). Static placeholder
// art for now — simulated_cameras.png and car_360.png aren't a real camera
// feed, there's no camera hardware wired into this project yet.
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

    function open() {
        opacity = 1
    }
    function close() {
        opacity = 0
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

    Image {
        anchors.fill: parent
        source: "qrc:/simulated_cameras.png"
    }

    Image {
        anchors.fill: parent
        source: "qrc:/car_360.png"
    }
}
