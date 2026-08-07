import QtQuick 2.15

// Full-screen time-set overlay, opened by tapping the clock on the main
// dash. Styled to sit alongside the gauge cluster (black background,
// bahnschrift/range fonts) rather than look like a separate app.
Item {
    id: root
    anchors.fill: parent
    visible: false
    z: 500

    property int hour12: 12   // 1..12
    property int minute: 0    // 0..59
    property bool isPM: false

    function open() {
        var now = new Date()
        var h = now.getHours()
        isPM = h >= 12
        hour12 = h % 12
        if (hour12 === 0) hour12 = 12
        minute = now.getMinutes()
        visible = true
    }

    function cancel() {
        visible = false
    }

    function save() {
        var hour24 = hour12 % 12
        if (isPM) hour24 += 12
        systemClock.setTime(hour24, minute)
        visible = false
    }

    // Swallow touches to the dash underneath while open.
    MouseArea {
        anchors.fill: parent
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Text {
        anchors.top: parent.top
        anchors.topMargin: 60
        anchors.horizontalCenter: parent.horizontalCenter
        font.family: bahnschriftFont.name
        font.pixelSize: 36
        color: "white"
        text: "SET TIME"
    }

    Row {
        anchors.centerIn: parent
        spacing: 40

        Column {
            spacing: 16
            anchors.verticalCenter: parent.verticalCenter

            StepButton {
                anchors.horizontalCenter: parent.horizontalCenter
                glyph: "▲"
                onClicked: root.hour12 = root.hour12 === 12 ? 1 : root.hour12 + 1
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                font.family: rangeFont.name
                font.pixelSize: 120
                color: "white"
                text: hour12.toString()
                horizontalAlignment: Text.AlignHCenter
            }
            StepButton {
                anchors.horizontalCenter: parent.horizontalCenter
                glyph: "▼"
                onClicked: root.hour12 = root.hour12 === 1 ? 12 : root.hour12 - 1
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            font.family: rangeFont.name
            font.pixelSize: 120
            color: "white"
            text: ":"
        }

        Column {
            spacing: 16
            anchors.verticalCenter: parent.verticalCenter

            StepButton {
                anchors.horizontalCenter: parent.horizontalCenter
                glyph: "▲"
                onClicked: root.minute = root.minute === 59 ? 0 : root.minute + 1
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                font.family: rangeFont.name
                font.pixelSize: 120
                color: "white"
                text: minute < 10 ? "0" + minute : minute.toString()
                horizontalAlignment: Text.AlignHCenter
            }
            StepButton {
                anchors.horizontalCenter: parent.horizontalCenter
                glyph: "▼"
                onClicked: root.minute = root.minute === 0 ? 59 : root.minute - 1
            }
        }

        Column {
            spacing: 16
            anchors.verticalCenter: parent.verticalCenter

            StepButton {
                anchors.horizontalCenter: parent.horizontalCenter
                glyph: "▲"
                onClicked: root.isPM = !root.isPM
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                font.family: bahnschriftFont.name
                font.pixelSize: 48
                color: "white"
                text: isPM ? "PM" : "AM"
                horizontalAlignment: Text.AlignHCenter
            }
            StepButton {
                anchors.horizontalCenter: parent.horizontalCenter
                glyph: "▼"
                onClicked: root.isPM = !root.isPM
            }
        }
    }

    Row {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 70
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 60

        DashButton {
            label: "CANCEL"
            onClicked: root.cancel()
        }
        DashButton {
            label: "SAVE"
            highlighted: true
            onClicked: root.save()
        }
    }

    component StepButton: Rectangle {
        id: stepBtn
        property alias glyph: glyphText.text
        signal clicked()

        width: 70
        height: 50
        radius: 8
        color: stepArea.pressed ? "#333333" : "#1a1a1a"
        border.color: "#555555"
        border.width: 1

        Text {
            id: glyphText
            anchors.centerIn: parent
            font.pixelSize: 20
            color: stepArea.pressed ? "white" : "#aaaaaa"
        }

        MouseArea {
            id: stepArea
            anchors.fill: parent
            onClicked: stepBtn.clicked()
        }
    }

    component DashButton: Rectangle {
        id: dashBtn
        property alias label: labelText.text
        property bool highlighted: false
        signal clicked()

        width: 200
        height: 70
        radius: 10
        color: dashArea.pressed ? (highlighted ? "#e0e0e0" : "#333333")
                                 : (highlighted ? "white" : "#1a1a1a")
        border.color: "#555555"
        border.width: 1

        Text {
            id: labelText
            anchors.centerIn: parent
            font.family: bahnschriftFont.name
            font.pixelSize: 28
            color: dashBtn.highlighted ? "black" : "white"
        }

        MouseArea {
            id: dashArea
            anchors.fill: parent
            onClicked: dashBtn.clicked()
        }
    }
}
