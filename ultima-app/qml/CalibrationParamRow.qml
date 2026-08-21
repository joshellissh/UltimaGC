import QtQuick 2.15

// One calibration parameter: label, live value readout, and -/+ steppers
// with press-and-hold repeat. Touch-friendly for the small numeric steps
// calibration tuning needs (fractions of an inch, half-degrees) — a raw
// drag-slider can't hit those precisely on a car dash touchscreen.
// `value` is a plain display binding (bind it straight to the
// CalibrationStore field); this item never writes that field itself —
// callers apply the edit in onValueEdited, which lets the store's own
// change-notification loop back around and update `value` for the next
// display frame.
Item {
    id: root
    property string label: ""
    property real value: 0
    property real step: 0.01
    property int decimals: 2
    property string unit: ""
    signal valueEdited(real newValue)

    height: 44
    width: parent ? parent.width : 300

    Text {
        text: root.label
        color: "white"
        font.pixelSize: 17
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
    }

    Row {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8

        Rectangle {
            width: 38; height: 38; radius: 6
            color: minusArea.pressed ? "#555555" : "#333333"
            Text { text: "-"; color: "white"; font.pixelSize: 20; anchors.centerIn: parent }
            MouseArea {
                id: minusArea
                anchors.fill: parent
                onPressed: { root.valueEdited(root.value - root.step); repeatTimer.dir = -1; repeatTimer.start() }
                onReleased: repeatTimer.stop()
                onCanceled: repeatTimer.stop()
            }
        }

        Text {
            text: root.value.toFixed(root.decimals) + root.unit
            color: "white"
            font.pixelSize: 17
            width: 92
            horizontalAlignment: Text.AlignHCenter
            anchors.verticalCenter: parent.verticalCenter
        }

        Rectangle {
            width: 38; height: 38; radius: 6
            color: plusArea.pressed ? "#555555" : "#333333"
            Text { text: "+"; color: "white"; font.pixelSize: 20; anchors.centerIn: parent }
            MouseArea {
                id: plusArea
                anchors.fill: parent
                onPressed: { root.valueEdited(root.value + root.step); repeatTimer.dir = 1; repeatTimer.start() }
                onReleased: repeatTimer.stop()
                onCanceled: repeatTimer.stop()
            }
        }
    }

    // Started on press, stopped on release/cancel — first step already
    // happened in onPressed above, so a plain tap-release never repeats.
    Timer {
        id: repeatTimer
        property int dir: 1
        interval: 120
        repeat: true
        onTriggered: root.valueEdited(root.value + dir * root.step)
    }
}
