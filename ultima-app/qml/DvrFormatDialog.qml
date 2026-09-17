import QtQuick 2.15

// Small modal dialog offering to format a USB drive that's plugged in but not
// set up for the dashcam DVR (see DashcamRecorder / DASHCAM.md). It auto-appears
// when such a drive is detected; FORMAT wipes it to exFAT labeled ULTIMA_DVR and
// CLOSE dismisses. After a format it shows a success or failure confirmation.
//
// All state lives in the C++ DashcamRecorder (context property `dashcam`):
//   formatPromptOpen  - whether this dialog should be visible at all
//   formatState       - idle | formatting | succeeded | failed
//   formatMessage     - the result line to show
// This file is self-contained (its own FontLoader + button component) so it
// touches none of the other, shipping screens — same pattern as
// CameraGridScreen/DiagnosticScreen each loading their own fonts.
Item {
    id: root
    anchors.fill: parent
    // Above the ~500 overlay screens (camera/diagnostic), below the z:8000
    // headlight dim and z:9000 FPS overlay — a modal decision that sits over
    // whatever is on screen but still dims with the headlights like everything.
    z: 6000

    // Set by main.qml to the dash's startup self-test flag: don't pop the
    // dialog during the ~2s boot sweep (cross-file id scope means it's passed
    // in as a property rather than read directly).
    property bool suppressed: false

    readonly property string state: dashcam.formatState
    visible: dashcam.formatPromptOpen && !suppressed

    FontLoader { id: bahnschriftFont; source: "qrc:/bahnschrift._semibold.ttf" }

    // Swallow every touch to the dash underneath while open (modal), and dim
    // the background — one alpha-blended quad, same cheap-compositor rationale
    // as main.qml's headlight dim.
    MouseArea { anchors.fill: parent }
    Rectangle { anchors.fill: parent; color: "black"; opacity: 0.6 }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: 760
        height: 360
        radius: 16
        color: "#141414"
        border.color: "#555555"
        border.width: 1

        Image {
            id: icon
            anchors.top: parent.top
            anchors.topMargin: 30
            anchors.horizontalCenter: parent.horizontalCenter
            width: 56
            height: 56
            sourceSize.width: 56
            sourceSize.height: 56
            source: "qrc:/camera_icon.png"
            visible: root.state !== "succeeded"

            // Rotating arc while formatting — a busy spinner drawn on a Canvas
            // (no external asset; Canvas + a transform rotation is safe on the
            // software Quick backend, same as SetTimeScreen's glyphs).
            Canvas {
                id: spinner
                anchors.centerIn: parent
                width: 76
                height: 76
                visible: root.state === "formatting"
                rotation: 0
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    ctx.lineWidth = 5
                    ctx.strokeStyle = "#ff3b30"
                    ctx.beginPath()
                    ctx.arc(width / 2, height / 2, width / 2 - 4, 0, Math.PI * 1.4)
                    ctx.stroke()
                }
                RotationAnimator on rotation {
                    running: root.state === "formatting"
                    from: 0
                    to: 360
                    duration: 900
                    loops: Animation.Infinite
                }
            }
        }

        // Green check for a completed format (drawn, not an asset).
        Canvas {
            id: check
            anchors.top: parent.top
            anchors.topMargin: 30
            anchors.horizontalCenter: parent.horizontalCenter
            width: 56
            height: 56
            visible: root.state === "succeeded"
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.lineWidth = 6
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                ctx.strokeStyle = "#34c759"
                ctx.beginPath()
                ctx.moveTo(width * 0.18, height * 0.55)
                ctx.lineTo(width * 0.42, height * 0.78)
                ctx.lineTo(width * 0.84, height * 0.24)
                ctx.stroke()
            }
        }

        Text {
            id: title
            anchors.top: icon.bottom
            anchors.topMargin: 18
            anchors.horizontalCenter: parent.horizontalCenter
            font.family: bahnschriftFont.name
            font.pixelSize: 30
            color: "white"
            text: root.state === "formatting" ? "FORMATTING DRIVE"
                : root.state === "succeeded"  ? "DRIVE READY"
                : root.state === "failed"     ? "FORMAT FAILED"
                :                               "USB DRIVE NOT SET UP"
        }

        Text {
            id: body
            anchors.top: title.bottom
            anchors.topMargin: 14
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 90
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.family: bahnschriftFont.name
            font.pixelSize: 20
            color: "#bbbbbb"
            text: root.state === "formatting"
                    ? "Setting up the drive for recording — this takes a moment."
                : root.state === "succeeded"
                    ? (dashcam.formatMessage !== "" ? dashcam.formatMessage
                                                    : "The drive is ready. Recording will start automatically.")
                : root.state === "failed"
                    ? (dashcam.formatMessage !== "" ? dashcam.formatMessage
                                                    : "The drive could not be formatted.")
                    : "This USB drive isn't set up for the dashcam. Format it now to start recording?\nThis erases everything on the drive."
        }

        Row {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 30
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 40

            // Prompt: FORMAT + CLOSE. Result: single acknowledge button.
            // Formatting: none (busy) — the flow can't be cancelled mid-mkfs.
            DialogButton {
                visible: root.state === "idle"
                label: "FORMAT"
                highlighted: true
                onClicked: dashcam.formatDrive()
            }
            DialogButton {
                visible: root.state === "idle" || root.state === "succeeded" || root.state === "failed"
                label: root.state === "succeeded" ? "OK" : "CLOSE"
                onClicked: dashcam.dismissFormatPrompt()
            }
        }
    }

    // Self-contained button (SetTimeScreen has its own inline DashButton; kept
    // separate here to avoid editing that shipping screen).
    component DialogButton: Rectangle {
        id: btn
        property alias label: btnText.text
        property bool highlighted: false
        signal clicked()

        width: 230
        height: 66
        radius: 10
        color: btnArea.pressed ? (highlighted ? "#e0e0e0" : "#333333")
                               : (highlighted ? "white" : "#1a1a1a")
        border.color: "#555555"
        border.width: 1

        Text {
            id: btnText
            anchors.centerIn: parent
            font.family: bahnschriftFont.name
            font.pixelSize: 26
            color: btn.highlighted ? "black" : "white"
        }

        MouseArea {
            id: btnArea
            anchors.fill: parent
            onClicked: btn.clicked()
        }
    }
}
