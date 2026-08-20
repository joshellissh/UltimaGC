#ifndef BLUETOOTHMANAGER_H
#define BLUETOOTHMANAGER_H

#include <QObject>
#include <QVariantList>
#include <QBluetoothLocalDevice>
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyAdvertisingData>
#include <QLowEnergyAdvertisingParameters>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyService>

class CanBus;

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
    // canBus is not owned — see main.cpp, where both are stack-constructed
    // with matching lifetimes. Used to actually drive the MCE18's AUX
    // outputs (Ultima AUX Control Service — see ANDROID-BLE-INTEGRATION.md)
    // once a central writes to that GATT service; may be null (e.g. a
    // future test harness), in which case AUX writes are accepted over BLE
    // but never reach CAN, matching how the rest of this class already
    // degrades gracefully with no adapter present.
    explicit BluetoothManager(CanBus *canBus, QObject *parent = nullptr);

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

    // Ultima AUX Control Service (see ANDROID-BLE-INTEGRATION.md). Built
    // once, right after m_peripheralController exists — addService() is
    // still unproven on this stack's BlueZ backend (see the class comment
    // above and the doc's own caveat), so this is the first real exercise
    // of that path, not something to assume just works.
    void ensureAuxService();
    // Fires on QLowEnergyService::characteristicChanged, which per Qt's
    // peripheral-role docs is how a remote central's write shows up
    // locally (there is no separate "incoming write request" signal in
    // this role — the characteristic's value is already updated by the
    // time this fires). Dispatches to the AUX1-4 write handling below;
    // ignored for any other characteristic (there are none writable in
    // this service besides AUX1-4).
    void onAuxCharacteristicChanged(const QLowEnergyCharacteristic &info, const QByteArray &value);
    // Validates one AUX write (index 0-3, matching CanBus::setAux()'s
    // indexing) against the range for that output, applies it to m_canBus
    // on success, and always reports the outcome via the Status
    // characteristic — see ANDROID-BLE-INTEGRATION.md's "don't swallow
    // garbage" principle, carried over from the old config-service design's
    // Trip Reset characteristic.
    void handleAuxWrite(int index, const QByteArray &value);
    // Pushes m_canBus's current AUX1-4 state to the AUX State
    // characteristic (Notify) — called after every accepted AUX write and
    // once on connect, so a freshly-connected app doesn't have to guess
    // starting state.
    void pushAuxState();
    // Writes "<index>:OK" or "<index>:ERROR:<message>" to the Status
    // characteristic (Notify) — see the GATT table in
    // ANDROID-BLE-INTEGRATION.md.
    void writeStatus(int index, bool ok, const QString &message = QString());

    QBluetoothLocalDevice *m_localDevice = nullptr;
    QLowEnergyController *m_peripheralController = nullptr;
    QLowEnergyService *m_auxService = nullptr;
    CanBus *m_canBus = nullptr;
    bool m_advertising = false;
    QString m_connectedDeviceName;
    QString m_connectedDeviceAddress;
    QString m_pendingPairAddress;
    QString m_pendingPairName;
    QString m_pendingPairCode;
    bool m_pendingPairNeedsConfirm = false;
};

#endif
