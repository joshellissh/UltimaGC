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

    property int year: 2024
    property int month: 1     // 1..12
    property int day: 1       // 1..daysInMonth(year, month)

    // JS Date's day-0-of-next-month trick: month here is already 1-based,
    // so passing it straight as the "next month" argument to the Date
    // constructor (which takes 0-based months) does the +1 for free.
    function daysInMonth(y, m) {
        return new Date(y, m, 0).getDate()
    }

    // Clamps day after a month/year step so e.g. Jan 31 -> Feb lands on
    // Feb 28/29 instead of silently overflowing into March.
    function clampDay() {
        var maxDay = daysInMonth(year, month)
        if (day > maxDay)
            day = maxDay
    }

    function open() {
        var now = new Date()
        var h = now.getHours()
        isPM = h >= 12
        hour12 = h % 12
        if (hour12 === 0) hour12 = 12
        minute = now.getMinutes()
        year = now.getFullYear()
        month = now.getMonth() + 1
        day = now.getDate()
        visible = true
    }

    function cancel() {
        visible = false
    }

    function save() {
        var hour24 = hour12 % 12
        if (isPM) hour24 += 12
        systemClock.setTime(year, month, day, hour24, minute)
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
        text: "SET DATE & TIME"
    }

    Column {
        anchors.centerIn: parent
        spacing: 16

        // Date row: month / day / year, sized down from the time row below
        // it (that's the primary control — it's the only thing the dash
        // itself ever displays) but using the same StepButton/column
        // pattern so the two rows read as one control, not two bolted
        // together.
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 24

            Column {
                spacing: 8
                anchors.verticalCenter: parent.verticalCenter

                StepButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    onClicked: {
                        root.month = root.month === 12 ? 1 : root.month + 1
                        root.clampDay()
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: rangeFont.name
                    font.pixelSize: 36
                    color: "white"
                    text: month < 10 ? "0" + month : month.toString()
                    horizontalAlignment: Text.AlignHCenter
                }
                StepButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    pointDown: true
                    onClicked: {
                        root.month = root.month === 1 ? 12 : root.month - 1
                        root.clampDay()
                    }
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                font.family: rangeFont.name
                font.pixelSize: 36
                color: "white"
                text: "/"
            }

            Column {
                spacing: 8
                anchors.verticalCenter: parent.verticalCenter

                StepButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    onClicked: root.day = root.day === root.daysInMonth(root.year, root.month) ? 1 : root.day + 1
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: rangeFont.name
                    font.pixelSize: 36
                    color: "white"
                    text: day < 10 ? "0" + day : day.toString()
                    horizontalAlignment: Text.AlignHCenter
                }
                StepButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    pointDown: true
                    onClicked: root.day = root.day === 1 ? root.daysInMonth(root.year, root.month) : root.day - 1
                }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                font.family: rangeFont.name
                font.pixelSize: 36
                color: "white"
                text: "/"
            }

            Column {
                spacing: 8
                anchors.verticalCenter: parent.verticalCenter

                StepButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    // Clamped, not wrapped, unlike month/day/hour/minute —
                    // a year rolling over from 2099 back to 2020 (or 12
                    // back to 1 for the month) reads as a natural odometer-
                    // style wrap, but a year doing that reads as broken.
                    onClicked: {
                        root.year = Math.min(root.year + 1, 2099)
                        root.clampDay()
                    }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    font.family: rangeFont.name
                    font.pixelSize: 36
                    color: "white"
                    text: year.toString()
                    horizontalAlignment: Text.AlignHCenter
                }
                StepButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    pointDown: true
                    onClicked: {
                        root.year = Math.max(root.year - 1, 2020)
                        root.clampDay()
                    }
                }
            }
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 40

            Column {
                spacing: 16
                anchors.verticalCenter: parent.verticalCenter

                StepButton {
                    anchors.horizontalCenter: parent.horizontalCenter
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
                    pointDown: true
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
                    pointDown: true
                    onClicked: root.minute = root.minute === 0 ? 59 : root.minute - 1
                }
            }

            Column {
                spacing: 16
                anchors.verticalCenter: parent.verticalCenter

                StepButton {
                    anchors.horizontalCenter: parent.horizontalCenter
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
                    pointDown: true
                    onClicked: root.isPM = !root.isPM
                }
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

    // Up/down triangle drawn on a Canvas rather than a Text glyph — neither
    // bundled font (bahnschrift, range) contains U+25B2/U+25BC, and this app
    // has no guaranteed system fallback font to borrow them from (the target's
    // Yocto image ships none at all), so the glyphs rendered as missing-glyph
    // boxes on target. Canvas is proven to render here already (see
    // boostRing in main.qml, same QT_QUICK_BACKEND=software path).
    component StepButton: Rectangle {
        id: stepBtn
        property bool pointDown: false
        signal clicked()

        width: 70
        height: 50
        radius: 8
        color: stepArea.pressed ? "#333333" : "#1a1a1a"
        border.color: "#555555"
        border.width: 1

        Canvas {
            id: glyphCanvas
            anchors.centerIn: parent
            width: 20
            height: 12
            property color triColor: stepArea.pressed ? "white" : "#aaaaaa"
            onTriColorChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                ctx.reset()
                ctx.fillStyle = triColor
                ctx.beginPath()
                if (stepBtn.pointDown) {
                    ctx.moveTo(0, 0)
                    ctx.lineTo(width, 0)
                    ctx.lineTo(width / 2, height)
                } else {
                    ctx.moveTo(0, height)
                    ctx.lineTo(width, height)
                    ctx.lineTo(width / 2, 0)
                }
                ctx.closePath()
                ctx.fill()
            }
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
