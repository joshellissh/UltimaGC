import QtQuick 2.15

// Bottom dot row shown on the three swipeable screens (CameraGridScreen,
// main.qml, DiagnosticScreen), so the dot order always matches the physical
// swipe layout: camera grid sits "left" (swipe right from main slides it in
// from the left edge), diagnostics sits "right" (swipe left slides it in
// from the right edge) — see each screen's open()/close() comments.
// Informational only, like the LINE design system reference this follows —
// not a tap target for jumping between screens.
Item {
    id: root
    property int currentIndex: 0
    property int count: 3

    width: row.width
    height: row.height

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 8

        Repeater {
            model: root.count
            delegate: Rectangle {
                readonly property bool active: index === root.currentIndex
                width: active ? 22 : 8
                height: 8
                radius: 4
                color: active ? "white" : "#4dffffff"
                border.width: 1
                border.color: "#66000000"

                Behavior on width {
                    NumberAnimation { duration: 200; easing.type: Easing.OutCubic }
                }
                Behavior on color {
                    ColorAnimation { duration: 200 }
                }
            }
        }
    }
}
