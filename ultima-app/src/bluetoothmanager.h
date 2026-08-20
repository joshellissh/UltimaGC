#ifndef BLUETOOTHMANAGER_H
#define BLUETOOTHMANAGER_H

#include <QObject>
#include <QVariantList>
#include <QBluetoothLocalDevice>
#include <QLowEnergyController>
#include <QLowEnergyAdvertisingData>
#include <QLowEnergyAdvertisingParameters>

// Backs the QML Bluetooth pairing screen (BluetoothScreen.qml), exposed as
// the `bluetooth` context property. The dash acts as a BLE *peripheral* —
// it advertises itself (see kLocalName below) — not a central that scans
// for and pairs with nearby devices. That's a deliberate choice, not the
// more obvious-sounding one: a phone's OS doesn't advertise itself as a
// connectable BLE peripheral, so a central-role scan from this dash would
// never show the driver's own phone in the list.
//
// Confirmed on real hardware (2026-08-19, HCI trace + seen from Windows)
// that advertising itself genuinely works, but neither iOS's nor Android's
// *built-in* Bluetooth settings will pair with a bare advertising-only BLE
// peripheral like this one — both need a dedicated companion app
// (CoreBluetooth / BluetoothLeScanner) to do anything with it. See
// ANDROID-BLE-INTEGRATION.md for the GATT service spec that app would need,
// and beagleplay-falcon/NOTES.md "Bluetooth via CC1352P7" for the fuller
// history (including the CC1352P7 network-processor dead end this
// USB-dongle approach replaced).
//
// Advertising is started once at boot on Linux (see main.cpp) and left
// running — it does not track BluetoothScreen being open or closed, so a
// companion app can connect at any time, not only while someone is
// standing at the touchscreen. BluetoothScreen just surfaces status and
// handles pairing-confirmation UI.
//
// Pairing-confirmation handling (QBluetoothLocalDevice) is adapter-level,
// not central/peripheral-specific — kept from the earlier scan-based
// design essentially unchanged, since BlueZ still needs a registered Agent
// to answer a numeric-comparison/PIN prompt regardless of which side
// initiated the connection.
//
// BeaglePlay's Bluetooth controller (the onboard CC1352P7, flashed with
// BLE HCI firmware, attached over UART) has no BR/EDR radio at all.
class BluetoothManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool advertising READ advertising NOTIFY advertisingChanged)
    Q_PROPERTY(QString connectedDeviceName READ connectedDeviceName NOTIFY connectionChanged)
    Q_PROPERTY(QString connectedDeviceAddress READ connectedDeviceAddress NOTIFY connectionChanged)
    Q_PROPERTY(QString localName READ localName CONSTANT)
    Q_PROPERTY(QString pendingPairAddress READ pendingPairAddress NOTIFY pairingRequestChanged)
    Q_PROPERTY(QString pendingPairName READ pendingPairName NOTIFY pairingRequestChanged)
    Q_PROPERTY(QString pendingPairCode READ pendingPairCode NOTIFY pairingRequestChanged)
    Q_PROPERTY(bool pendingPairNeedsConfirm READ pendingPairNeedsConfirm NOTIFY pairingRequestChanged)

public:
    explicit BluetoothManager(QObject *parent = nullptr);

    bool available() const;
    bool advertising() const { return m_advertising; }
    QString connectedDeviceName() const { return m_connectedDeviceName; }
    QString connectedDeviceAddress() const { return m_connectedDeviceAddress; }
    QString localName() const;
    QString pendingPairAddress() const { return m_pendingPairAddress; }
    QString pendingPairName() const { return m_pendingPairName; }
    QString pendingPairCode() const { return m_pendingPairCode; }
    bool pendingPairNeedsConfirm() const { return m_pendingPairNeedsConfirm; }

    Q_INVOKABLE void startAdvertising();
    Q_INVOKABLE void stopAdvertising();
    // Answers a pending numeric-comparison pairing prompt (see
    // pendingPairNeedsConfirm above) — no-op if nothing is pending.
    Q_INVOKABLE void confirmPairing(bool accept);

signals:
    void availableChanged();
    void advertisingChanged();
    void connectionChanged();
    void pairingRequestChanged();

private:
    // Both lazy: constructed on first real use, not at app startup. Merely
    // *constructing* a QBluetoothLocalDevice/QLowEnergyController is enough
    // to trigger macOS's CoreBluetooth permission prompt on the dev build —
    // confirmed live, it fired at process launch before anything ever
    // touched the icon. Target hardware (BlueZ) has no such gate, but
    // staying lazy either way means a drive that never opens this screen
    // never touches the radio.
    void ensureLocalDevice();
    void ensurePeripheralController();
    void clearPendingPair();

    QBluetoothLocalDevice *m_localDevice = nullptr;
    QLowEnergyController *m_peripheralController = nullptr;
    bool m_advertising = false;
    QString m_connectedDeviceName;
    QString m_connectedDeviceAddress;
    QString m_pendingPairAddress;
    QString m_pendingPairName;
    QString m_pendingPairCode;
    bool m_pendingPairNeedsConfirm = false;
};

#endif
