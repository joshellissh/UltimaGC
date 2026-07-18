import QtQuick 2.15

// Segmented shift-light bar: fills green -> amber -> red with rpm, then
// blinks every segment together once rpm reaches the redline threshold —
// the same 300ms cadence main.qml already uses for the warning icons.
Item {
    id: bar

    property real rpm: 0
    property real maxRpm: 8000
    property real redlineRpm: 7000
    property int segmentCount: 12
    property bool flash: true // driven by main.qml's shared _warnFlash timer

    readonly property real frac: Math.max(0, Math.min(1, rpm / maxRpm))
    readonly property bool atRedline: rpm >= redlineRpm

    Row {
        anchors.fill: parent
        spacing: 4

        Repeater {
            model: bar.segmentCount

            Rectangle {
                width: (bar.width - (bar.segmentCount - 1) * 4) / bar.segmentCount
                height: bar.height
                radius: 2

                property real segStart: index / bar.segmentCount
                property bool lit: bar.frac >= segStart

                color: {
                    if (bar.atRedline) return bar.flash ? "#ff2020" : "#3a1010"
                    if (!lit) return "#222222"
                    if (segStart >= 0.83) return "#ff2020"
                    if (segStart >= 0.6) return "#ff9900"
                    return "#30d030"
                }
            }
        }
    }
}
