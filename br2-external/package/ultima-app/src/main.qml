import QtQuick 2.15
import QtQuick.Window 2.15

Window {
    id: root
    visibility: Window.FullScreen
    color: "black"

    // Hello title
    Text {
        id: title
        text: "Hello, Ultima"
        color: "white"
        font.pixelSize: 72
        font.weight: Font.Light
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: parent.height * 0.25
    }

    // Live clock
    Text {
        id: clock
        color: "#aaaaaa"
        font.pixelSize: 48
        font.weight: Font.Light
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: title.bottom
        anchors.topMargin: 40

        Timer {
            interval: 1000
            running: true
            repeat: true
            triggeredOnStart: true
            onTriggered: {
                var now = new Date()
                clock.text = Qt.formatTime(now, "hh:mm:ss AP")
            }
        }
    }

    // Touch feedback
    Text {
        id: touchLabel
        color: "#666666"
        font.pixelSize: 24
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 60
        opacity: 0

        Behavior on opacity {
            NumberAnimation { duration: 300 }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: function(mouse) {
            touchLabel.text = "Touch: " + Math.round(mouse.x) + ", " + Math.round(mouse.y)
            touchLabel.opacity = 1.0
            fadeTimer.restart()
        }
    }

    Timer {
        id: fadeTimer
        interval: 2000
        onTriggered: touchLabel.opacity = 0
    }
}
