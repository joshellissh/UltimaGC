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
        // Bonded-device snapshot can go stale between visits (a phone
        // bonded, or was forgotten from bluetoothctl directly) — refresh
        // every time the driver actually looks at this screen rather than
        // only reacting to pairing events this app itself drove.
        bluetooth.refreshPairedDevices()
    }

    // Referenced below (font.family: bahnschriftFont.name / rangeFont.name)
    // without a local declaration until now — this screen relied on ids
    // from main.qml that a separately-instantiated component (see
    // `BluetoothScreen { id: bluetoothScreen }` in main.qml) never actually
    // has in scope, so every Text here was silently falling back to the
    // default system font. DiagnosticScreen.qml already declares its own
    // copies for the same reason; matching that pattern here rather than
    // leaving it broken given how much text this screen now has.
    FontLoader { id: bahnschriftFont; source: "qrc:/bahnschrift._semibold.ttf" }
    FontLoader { id: rangeFont; source: "qrc:/range.regular.ttf" }

    function close() {
        // Belt-and-suspenders alongside BluetoothManager's own
        // kPairingModeTimeoutMs: closing this screen ends an open pairing
        // window immediately rather than leaving it running in the
        // background for whatever's left of the timeout — the window is
        // only supposed to be open while the driver is actively at this
        // screen for exactly this purpose.
        bluetooth.exitPairingMode()
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
        id: statusText
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

    // Pairing is closed by default (see BluetoothManager::startAdvertising's
    // setAdapterPairable(m_pairingModeActive)) — an already-bonded phone
    // still connects and reconnects any time regardless of this, since
    // reconnecting never goes through bonding again. This row is only about
    // letting a *new* device bond, and only for a limited window opened
    // deliberately here.
    Text {
        anchors.top: statusText.bottom
        anchors.topMargin: 10
        anchors.horizontalCenter: parent.horizontalCenter
        font.family: bahnschriftFont.name
        font.pixelSize: 16
        color: "#4caf50"
        visible: bluetooth.pairingModeActive
        text: "Pairing mode open — waiting for a new device..."
    }

    // Left column (x 80-640) — mirrors the right column's cards below
    // (x 960-1520 on a 1600-wide screen) with a 320px gap between them, so
    // the two never collide regardless of content width. This used to be
    // plain anchors.centerIn — safe back when this screen's whole middle
    // was empty, but the AUX/paired-devices cards added 2026-08-20 claim
    // the right half, so this and connectedBox below were moved out of the
    // center to make room rather than risk painting under those cards.
    Text {
        id: instructionalText
        anchors.left: parent.left
        anchors.leftMargin: 80
        anchors.bottom: connectedBox.top
        anchors.bottomMargin: 20
        font.family: bahnschriftFont.name
        font.pixelSize: 20
        color: "#555555"
        visible: bluetooth.available
        text: "Visible to nearby BLE scanners (not your phone's Bluetooth settings)"
    }

    // Connected-device status — the only "list" this screen has, since the
    // dash only ever talks to one central at a time as a peripheral.
    Rectangle {
        id: connectedBox
        anchors.left: parent.left
        anchors.leftMargin: 80
        anchors.verticalCenter: parent.verticalCenter
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

    // Live AUX1-4 status — reads straight off `sim` (the CanBus context
    // property, see canbus.h's auxStates), which is what actually changes
    // the instant a connected companion app writes an AUX characteristic
    // (BluetoothManager::handleAuxWrite -> CanBus::setAux -> auxStateChanged).
    // This is the whole point of this card: watching it react live is how
    // the phone app's writes get verified from the dash side without a
    // scope on the physical output. This and pairedCard below claim the
    // right half of the screen (see the left-column comment above for why
    // connectedBox/instructionalText moved out of dead center to make room).
    Rectangle {
        id: auxCard
        // topMargin 190 (not e.g. 130) clears the title/status/pairing-mode
        // texts above regardless of their rendered width — those are
        // horizontally centered (x ~800) while this card starts at x 960,
        // so only vertical separation matters here, not horizontal.
        anchors.top: parent.top
        anchors.topMargin: 190
        anchors.right: parent.right
        anchors.rightMargin: 80
        width: 560
        height: 160
        radius: 10
        color: "#1a1a1a"
        border.color: "#3a3a3a"
        border.width: 1
        visible: bluetooth.available

        Column {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 16

            Text {
                font.family: bahnschriftFont.name
                font.pixelSize: 18
                color: "#888888"
                text: "AUX OUTPUTS — live status"
            }

            Row {
                width: parent.width
                spacing: 20

                Repeater {
                    model: 4

                    delegate: Column {
                        width: (auxCard.width - 40 - 60) / 4
                        spacing: 6

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            font.family: bahnschriftFont.name
                            font.pixelSize: 16
                            color: "#aaaaaa"
                            text: "AUX" + (index + 1)
                        }

                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 14
                            height: 14
                            radius: 7
                            color: sim.auxStates[index] > 0 ? "#4caf50" : "#3a3a3a"
                        }

                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            font.family: rangeFont.name
                            font.pixelSize: 18
                            color: sim.auxStates[index] > 0 ? "#4caf50" : "#666666"
                            // AUX4 (index 3) is PWM 0-100%; AUX1-3 are plain on/off
                            // — see ANDROID-BLE-INTEGRATION.md's GATT table.
                            text: index === 3 ? (sim.auxStates[3] + "%") : (sim.auxStates[index] > 0 ? "ON" : "OFF")
                        }
                    }
                }
            }
        }
    }

    // Bonded devices known to the adapter, with a per-device unpair action —
    // see BluetoothManager::refreshPairedDevices()/forgetDevice(). Distinct
    // from the connected-device box above: a phone can be bonded but not
    // currently connected (car off, phone out of range, app closed), and
    // "forget" needs to reach those too, not just whichever one is live.
    Rectangle {
        id: pairedCard
        // Ends at 190+160+20+190=560, 20px clear of the PAIR NEW
        // DEVICE/CLOSE row below (bottomMargin 70 + height 70 = starts at
        // y 580 on a 720-tall screen) — that row is centered (x ~526-1074
        // at its widest) so it does reach into this card's x range
        // (960-1520), making the vertical gap load-bearing, not optional.
        anchors.top: auxCard.bottom
        anchors.topMargin: 20
        anchors.right: parent.right
        anchors.rightMargin: 80
        width: 560
        height: 190
        radius: 10
        color: "#1a1a1a"
        border.color: "#3a3a3a"
        border.width: 1
        visible: bluetooth.available

        Text {
            id: pairedHeader
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 20
            font.family: bahnschriftFont.name
            font.pixelSize: 18
            color: "#888888"
            text: "PAIRED DEVICES"
        }

        Text {
            anchors.centerIn: parent
            visible: bluetooth.pairedDevices.length === 0
            font.family: bahnschriftFont.name
            font.pixelSize: 16
            color: "#555555"
            text: "No paired devices"
        }

        ListView {
            anchors.top: pairedHeader.bottom
            anchors.topMargin: 12
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 20
            clip: true
            spacing: 8
            visible: bluetooth.pairedDevices.length > 0
            model: bluetooth.pairedDevices

            delegate: Rectangle {
                width: ListView.view.width
                height: 56
                radius: 8
                color: "#222222"
                border.width: 1
                border.color: modelData.address === bluetooth.connectedDeviceAddress ? "#4caf50" : "#3a3a3a"

                Column {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: forgetBtn.left
                    anchors.rightMargin: 12
                    spacing: 2

                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        font.family: bahnschriftFont.name
                        font.pixelSize: 16
                        color: "white"
                        text: modelData.name || modelData.address
                    }
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        font.family: bahnschriftFont.name
                        font.pixelSize: 12
                        color: modelData.address === bluetooth.connectedDeviceAddress ? "#4caf50" : "#666666"
                        text: modelData.address === bluetooth.connectedDeviceAddress ? "Connected" : modelData.address
                    }
                }

                // Deliberately styled apart from DashButton (red accent,
                // smaller) — this is a destructive per-row action, not a
                // primary screen action like PAIR NEW DEVICE/CLOSE below.
                Rectangle {
                    id: forgetBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 90
                    height: 36
                    radius: 8
                    color: forgetArea.pressed ? "#5a2020" : "#3a1a1a"
                    border.color: "#c0392b"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        font.family: bahnschriftFont.name
                        font.pixelSize: 13
                        color: "#e74c3c"
                        text: "FORGET"
                    }

                    MouseArea {
                        id: forgetArea
                        anchors.fill: parent
                        onClicked: bluetooth.forgetDevice(modelData.address)
                    }
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
            label: bluetooth.pairingModeActive ? "STOP PAIRING" : "PAIR NEW DEVICE"
            highlighted: bluetooth.pairingModeActive
            onClicked: bluetooth.pairingModeActive ? bluetooth.exitPairingMode() : bluetooth.enterPairingMode()
        }
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

        // Fixed 200 used to clip longer labels ("PAIR NEW DEVICE",
        // "STOP PAIRING") past the button's edge — grow to fit the actual
        // rendered text instead, with 200 as a floor so CLOSE/CONFIRM/
        // REJECT keep their existing size rather than shrinking to fit.
        width: Math.max(200, labelText.implicitWidth + 48)
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
