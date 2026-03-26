import QtQuick 2.15

Item {
    id: infoBar
    height: 50

    property real speed: 0
    property real fuelConsumption: 0
    property int gear: 0
    property real totalOdo: 0
    property real tripOdo: 0
    property real outsideTemp: 0
    property string driveMode: "ECO PRO"

    // Background with subtle gradient
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#181818" }
            GradientStop { position: 1.0; color: "#0c0c0c" }
        }
    }

    // Top border line with subtle glow
    Rectangle {
        width: parent.width
        height: 1
        color: "#333333"
        anchors.top: parent.top
    }
    Rectangle {
        width: parent.width
        height: 3
        anchors.top: parent.top
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: "#20ffffff" }
            GradientStop { position: 1.0; color: "#00ffffff" }
        }
    }

    Row {
        anchors.centerIn: parent
        spacing: 0
        height: parent.height

        // Clock
        Item {
            width: clockText.width + 40
            height: parent.height
            Text {
                id: clockText
                anchors.centerIn: parent
                color: "#DDDDDD"
                font.pixelSize: 18
                font.family: "Helvetica Neue"
                font.letterSpacing: 1

                Timer {
                    interval: 1000
                    running: true
                    repeat: true
                    triggeredOnStart: true
                    onTriggered: {
                        var now = new Date()
                        clockText.text = Qt.formatTime(now, "hh:mm")
                    }
                }
            }
        }

        // Separator
        Rectangle { width: 1; height: 20; color: "#333333"; anchors.verticalCenter: parent.verticalCenter }

        // Total odo
        Item {
            width: totalText.width + 40
            height: parent.height
            Text {
                id: totalText
                anchors.centerIn: parent
                text: "TOTAL  " + Math.floor(totalOdo).toString().padStart(6, '0') + " km"
                color: "#888888"
                font.pixelSize: 14
                font.family: "Courier"
                font.letterSpacing: 0.5
            }
        }

        Rectangle { width: 1; height: 20; color: "#333333"; anchors.verticalCenter: parent.verticalCenter }

        // Trip odo
        Item {
            width: tripText.width + 40
            height: parent.height
            Text {
                id: tripText
                anchors.centerIn: parent
                text: "TRIP  " + tripOdo.toFixed(1).padStart(7, '0') + " km"
                color: "#888888"
                font.pixelSize: 14
                font.family: "Courier"
                font.letterSpacing: 0.5
            }
        }

        Rectangle { width: 1; height: 20; color: "#333333"; anchors.verticalCenter: parent.verticalCenter }

        // Outside temp
        Item {
            width: tempText.width + 40
            height: parent.height
            Text {
                id: tempText
                anchors.centerIn: parent
                text: outsideTemp.toFixed(1) + " \u00B0C"
                color: "#888888"
                font.pixelSize: 14
                font.family: "Courier"
                font.letterSpacing: 0.5
            }
        }

        Rectangle { width: 1; height: 20; color: "#333333"; anchors.verticalCenter: parent.verticalCenter }

        // Drive mode with glow
        Item {
            width: modeText.width + 40
            height: parent.height

            // Glow behind text
            Rectangle {
                anchors.centerIn: modeText
                width: modeText.width + 16
                height: 24
                radius: 4
                color: Qt.rgba(0, 0.808, 0.82, 0.08)
                border.color: Qt.rgba(0, 0.808, 0.82, 0.15)
                border.width: 1
            }

            Text {
                id: modeText
                anchors.centerIn: parent
                text: driveMode
                color: "#00CED1"
                font.pixelSize: 15
                font.bold: true
                font.letterSpacing: 2
            }
        }

        Rectangle { width: 1; height: 20; color: "#333333"; anchors.verticalCenter: parent.verticalCenter }

        // Gear indicator
        Item {
            width: 60
            height: parent.height
            Text {
                anchors.centerIn: parent
                text: gear === 0 ? "N" : gear.toString()
                color: gear === 0 ? "#666666" : "white"
                font.pixelSize: 26
                font.bold: true
                font.family: "Helvetica Neue"
            }
        }
    }
}
