import QtQuick 2.15

// Bluetooth pairing overlay, opened by tapping the small Bluetooth glyph in
// the bottom-right corner of the main dash. Styled to match SetTimeScreen
// (full-screen black overlay, bahnschrift/range fonts, DashButton-style
// controls) rather than look like a separate app.
//
// Backed by the `bluetooth` context property (BluetoothManager, see
// bluetoothmanager.h). The dash advertises itself as a BLE peripheral
// rather than scanning for one, so this screen shows discoverable/connected
// status rather than a scan-and-pick device list. See
// beagleplay-falcon/NOTES.md "Bluetooth via CC1352P7" for why (a phone
// doesn't advertise itself as a connectable BLE peripheral, so a scan from
// this side would never find it).
//
// Confirmed on real hardware (2026-08-19, HCI trace + seen from Windows)
// that the advertising itself genuinely works. But neither iOS nor Android's
// *built-in* Bluetooth settings will pair with a bare advertising-only BLE
// peripheral like this one — both need a dedicated app on the phone
// (CoreBluetooth / BluetoothLeScanner) to do anything with it, which doesn't
// exist yet. Earlier assumption that Android's Settings would just work was
// wrong — corrected after testing against a real Android phone, not before.
// Don't put "connect from your phone's Bluetooth settings" back in the UI
// text below without re-verifying that's actually true again.
//
// Advertising is always on (started once at boot on Linux, see main.cpp) —
// it does NOT start/stop with this screen. open()/close() below only toggle
// this status/pairing-confirmation overlay, so a companion app can connect
// at any time, including while nobody is looking at the touchscreen.
Item {
    id: root
    anchors.fill: parent
    visible: false
    z: 500

    function open() {
        visible = true
    }

    function close() {
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
        id: titleText
        anchors.top: parent.top
        anchors.topMargin: 60
        anchors.horizontalCenter: parent.horizontalCenter
        font.family: bahnschriftFont.name
        font.pixelSize: 36
        color: "white"
        text: "BLUETOOTH"
    }

    Text {
        anchors.top: titleText.bottom
        anchors.topMargin: 12
        anchors.horizontalCenter: parent.horizontalCenter
        font.family: bahnschriftFont.name
        font.pixelSize: 18
        color: "#888888"
        text: {
            if (!bluetooth.available) return "No adapter found"
            if (!bluetooth.advertising) return "Starting..."
            return "Discoverable as “" + bluetooth.localName + "”"
        }
    }

    Text {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -60
        font.family: bahnschriftFont.name
        font.pixelSize: 20
        color: "#555555"
        visible: bluetooth.available
        text: "Visible to nearby BLE scanners (not your phone's Bluetooth settings)"
    }

    // Connected-device status — the only "list" this screen has, since the
    // dash only ever talks to one central at a time as a peripheral.
    Rectangle {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: 40
        width: 560
        height: 80
        radius: 10
        color: "#1a1a1a"
        border.color: bluetooth.connectedDeviceAddress !== "" ? "#4caf50" : "#3a3a3a"
        border.width: 1
        visible: bluetooth.available

        Text {
            anchors.centerIn: parent
            font.family: bahnschriftFont.name
            font.pixelSize: 22
            color: bluetooth.connectedDeviceAddress !== "" ? "#4caf50" : "#777777"
            text: bluetooth.connectedDeviceAddress !== ""
                ? "Connected: " + (bluetooth.connectedDeviceName || bluetooth.connectedDeviceAddress)
                : "Waiting for a connection..."
        }
    }

    Row {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 70
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 60

        DashButton {
            label: "CLOSE"
            onClicked: root.close()
        }
    }

    // Pairing-request panel — appears whenever the connected phone (or the
    // dash itself) initiates pairing/bonding after connecting (see
    // BluetoothManager's pairingDisplayConfirmation/pairingDisplayPinCode
    // handlers — adapter-level, not specific to the peripheral role above).
    // Only pendingPairNeedsConfirm shows Accept/Reject: a plain PIN display
    // needs no action here, just a wait.
    Rectangle {
        anchors.centerIn: parent
        width: 500
        height: 220
        radius: 14
        color: "#111111"
        border.color: "#555555"
        border.width: 1
        visible: bluetooth.pendingPairAddress !== ""

        Column {
            anchors.centerIn: parent
            spacing: 18

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                font.family: bahnschriftFont.name
                font.pixelSize: 22
                color: "white"
                text: "Pairing with " + bluetooth.pendingPairName
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: bluetooth.pendingPairCode !== ""
                font.family: rangeFont.name
                font.pixelSize: 40
                color: "white"
                text: bluetooth.pendingPairCode
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !bluetooth.pendingPairNeedsConfirm
                font.family: bahnschriftFont.name
                font.pixelSize: 16
                color: "#888888"
                text: "Confirm on your device if prompted..."
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 40
                visible: bluetooth.pendingPairNeedsConfirm

                DashButton {
                    label: "REJECT"
                    onClicked: bluetooth.confirmPairing(false)
                }
                DashButton {
                    label: "CONFIRM"
                    highlighted: true
                    onClicked: bluetooth.confirmPairing(true)
                }
            }
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
