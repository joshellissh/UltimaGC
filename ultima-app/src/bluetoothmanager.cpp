#include "bluetoothmanager.h"
#include "canbus.h"

#include <QTimer>
#include <QLowEnergyServiceData>
#include <QLowEnergyCharacteristicData>
#include <QLowEnergyDescriptorData>
#include <cstdio>

namespace {
// Reuses the name already chosen for the (since-reverted) WiFi AP — see
// beagleplay-falcon/NOTES.md "WiFi AP" — rather than invent a second brand
// name for the same car.
const char *const kLocalName = "Ultima RS";

// Ultima AUX Control Service (see ANDROID-BLE-INTEGRATION.md) — freshly
// generated UUIDs, not reused from anywhere else. Every characteristic UUID
// is the service UUID with only the first group changed; the doc explains
// the scheme in full.
const QBluetoothUuid kAuxServiceUuid(QStringLiteral("f6090001-ad4f-48f1-9b7f-a8d8a68b8c0b"));
const QBluetoothUuid kAux1CharUuid(QStringLiteral("f6090002-ad4f-48f1-9b7f-a8d8a68b8c0b"));
const QBluetoothUuid kAux2CharUuid(QStringLiteral("f6090003-ad4f-48f1-9b7f-a8d8a68b8c0b"));
const QBluetoothUuid kAux3CharUuid(QStringLiteral("f6090004-ad4f-48f1-9b7f-a8d8a68b8c0b"));
const QBluetoothUuid kAux4CharUuid(QStringLiteral("f6090005-ad4f-48f1-9b7f-a8d8a68b8c0b"));
const QBluetoothUuid kAuxStateCharUuid(QStringLiteral("f6090006-ad4f-48f1-9b7f-a8d8a68b8c0b"));
const QBluetoothUuid kStatusCharUuid(QStringLiteral("f6090007-ad4f-48f1-9b7f-a8d8a68b8c0b"));

// index (0-3, matching CanBus::setAux()) -> that AUX's write characteristic
// UUID, in table order — used both to build the service and to map an
// incoming characteristicChanged() back to an index.
const QBluetoothUuid *const kAuxCharUuids[4] = {
    &kAux1CharUuid, &kAux2CharUuid, &kAux3CharUuid, &kAux4CharUuid
};

// A Client Characteristic Configuration Descriptor is what lets a central
// subscribe to Notify — without one, writeCharacteristic() below still
// updates the local value but never actually reaches a subscriber. Starts
// at "notifications disabled" (0x0000); the central enables it by writing
// 0x0001, which Qt's peripheral role handles internally (this app never
// sees that write, matching Qt's own heartrate-server peripheral example).
QLowEnergyDescriptorData makeCccd()
{
    QLowEnergyDescriptorData cccd(QBluetoothUuid(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration),
                                   QByteArray(2, char(0)));
    cccd.setReadPermissions(true);
    cccd.setWritePermissions(true);
    return cccd;
}

// AUX1-4 write characteristics all share this shape: write-only, bonded
// link required. A successful write directly energizes a physical output
// in the car (see ANDROID-BLE-INTEGRATION.md's security-model note), so
// this is treated as more sensitive than the old config design's plain
// writes, not less.
QLowEnergyCharacteristicData makeAuxWriteChar(const QBluetoothUuid &uuid)
{
    QLowEnergyCharacteristicData data;
    data.setUuid(uuid);
    data.setProperties(QLowEnergyCharacteristic::Write);
    data.setValue(QByteArray(1, char(0)));
    data.setWriteConstraints(QBluetooth::AttAccessConstraint::AttEncryptionRequired);
    return data;
}
}

BluetoothManager::BluetoothManager(CanBus *canBus, QObject *parent)
    : QObject(parent), m_canBus(canBus)
{
}

void BluetoothManager::ensureLocalDevice()
{
    if (m_localDevice)
        return;
    m_localDevice = new QBluetoothLocalDevice(this);
    connect(m_localDevice, &QBluetoothLocalDevice::pairingFinished,
            this, [this](const QBluetoothAddress &address, QBluetoothLocalDevice::Pairing pairing) {
        bool succeeded = pairing != QBluetoothLocalDevice::Unpaired;
        fprintf(stderr, "[bluetooth] pairing %s with %s\n",
                succeeded ? "succeeded" : "failed", qPrintable(address.toString()));
        clearPendingPair();
    });
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Qt6 dropped QBluetoothLocalDevice's confirmation/PIN-display API
    // entirely (pairing confirmation is handled implicitly by the platform
    // backend there) — this half of the flow only exists, and is only
    // needed, on the Qt5/BlueZ target build. The mac dev build (Qt6, via
    // Homebrew) never triggers it; nothing here is reachable without real
    // Bluetooth hardware to test against anyway.
    connect(m_localDevice, &QBluetoothLocalDevice::pairingDisplayConfirmation,
            this, [this](const QBluetoothAddress &address, QString pin) {
        m_pendingPairAddress = address.toString();
        m_pendingPairName = m_connectedDeviceAddress == m_pendingPairAddress && !m_connectedDeviceName.isEmpty()
            ? m_connectedDeviceName : m_pendingPairAddress;
        m_pendingPairCode = pin;
        m_pendingPairNeedsConfirm = true;
        emit pairingRequestChanged();
    });
    connect(m_localDevice, &QBluetoothLocalDevice::pairingDisplayPinCode,
            this, [this](const QBluetoothAddress &address, QString pin) {
        m_pendingPairAddress = address.toString();
        m_pendingPairName = m_connectedDeviceAddress == m_pendingPairAddress && !m_connectedDeviceName.isEmpty()
            ? m_connectedDeviceName : m_pendingPairAddress;
        m_pendingPairCode = pin;
        m_pendingPairNeedsConfirm = false;
        emit pairingRequestChanged();
    });
#endif
    emit availableChanged();
}

void BluetoothManager::ensurePeripheralController()
{
    if (m_peripheralController)
        return;
    m_peripheralController = QLowEnergyController::createPeripheral(this);
    connect(m_peripheralController, &QLowEnergyController::connected, this, [this]() {
        m_connectedDeviceAddress = m_peripheralController->remoteAddress().toString();
        m_connectedDeviceName = m_peripheralController->remoteName();
        emit connectionChanged();
        // Sync a freshly-connected app's AUX toggles/slider to actual
        // dash-side state instead of leaving it to guess — see
        // ANDROID-BLE-INTEGRATION.md's "AUX State" characteristic.
        pushAuxState();
    });
    connect(m_peripheralController, &QLowEnergyController::disconnected, this, [this]() {
        m_connectedDeviceAddress.clear();
        m_connectedDeviceName.clear();
        emit connectionChanged();
        // Failsafe: force every AUX output off the moment the link drops,
        // rather than trusting an unconfirmed MCE18-side receive timeout —
        // see ANDROID-BLE-INTEGRATION.md "Failsafe on BLE disconnect" for
        // why this is the primary safety mechanism, not a backstop.
        if (m_canBus)
            m_canBus->allAuxOff();
        pushAuxState();
        // A central disconnecting doesn't stop advertising on its own —
        // restart it so the next phone can find and connect to the dash
        // without the driver having to reopen this screen.
        if (m_advertising)
            startAdvertising();
    });
    ensureAuxService();
}

void BluetoothManager::ensureAuxService()
{
    if (m_auxService)
        return;

    QLowEnergyServiceData serviceData;
    serviceData.setType(QLowEnergyServiceData::ServiceTypePrimary);
    serviceData.setUuid(kAuxServiceUuid);
    serviceData.addCharacteristic(makeAuxWriteChar(kAux1CharUuid));
    serviceData.addCharacteristic(makeAuxWriteChar(kAux2CharUuid));
    serviceData.addCharacteristic(makeAuxWriteChar(kAux3CharUuid));
    serviceData.addCharacteristic(makeAuxWriteChar(kAux4CharUuid));

    QLowEnergyCharacteristicData auxState;
    auxState.setUuid(kAuxStateCharUuid);
    auxState.setProperties(QLowEnergyCharacteristic::Read | QLowEnergyCharacteristic::Notify);
    auxState.setValue(QByteArray(4, char(0)));
    auxState.addDescriptor(makeCccd());
    serviceData.addCharacteristic(auxState);

    QLowEnergyCharacteristicData status;
    status.setUuid(kStatusCharUuid);
    status.setProperties(QLowEnergyCharacteristic::Read | QLowEnergyCharacteristic::Notify);
    status.setValue(QByteArray());
    status.addDescriptor(makeCccd());
    serviceData.addCharacteristic(status);

    // Unproven on this stack — see the class comment in bluetoothmanager.h
    // and ANDROID-BLE-INTEGRATION.md's repeated caveat: only advertising +
    // connection have been hardware-verified so far, not a local GATT
    // server. addService() can return nullptr; fall through and log rather
    // than crash if it does, same defensive posture as the rest of this
    // class around adapter/controller failures.
    m_auxService = m_peripheralController->addService(serviceData, this);
    if (!m_auxService) {
        fprintf(stderr, "[bluetooth] addService() failed — AUX Control Service not available "
                        "(see ANDROID-BLE-INTEGRATION.md, addService() is unproven on this stack)\n");
        return;
    }
    connect(m_auxService, &QLowEnergyService::characteristicChanged,
            this, &BluetoothManager::onAuxCharacteristicChanged);
    fprintf(stderr, "[bluetooth] Ultima AUX Control Service registered\n");
}

void BluetoothManager::onAuxCharacteristicChanged(const QLowEnergyCharacteristic &info, const QByteArray &value)
{
    const QBluetoothUuid uuid = info.uuid();
    for (int i = 0; i < 4; ++i) {
        if (uuid == *kAuxCharUuids[i]) {
            handleAuxWrite(i, value);
            return;
        }
    }
    // AUX State and Status are Read/Notify only — nothing else in this
    // service is writable, so no other UUID should ever reach here.
}

void BluetoothManager::handleAuxWrite(int index, const QByteArray &value)
{
    // charIndex in the Status characteristic's "<index>:OK"/"ERROR" wire
    // format is 1-based, matching the "#" column in
    // ANDROID-BLE-INTEGRATION.md's GATT table (AUX1 is #1, not #0).
    const int statusIndex = index + 1;
    if (value.size() != 1) {
        writeStatus(statusIndex, false,
                    QStringLiteral("expected 1 byte, got %1").arg(value.size()));
        return;
    }
    int raw = quint8(value.at(0));
    // AUX4 (index 3) accepts 0-100 PWM duty; AUX1-3 accept only 0 or 1.
    // Reject out-of-range values via Status rather than silently clamping
    // — see ANDROID-BLE-INTEGRATION.md's "don't swallow garbage" note.
    bool valid = (index == 3) ? (raw >= 0 && raw <= 100) : (raw == 0 || raw == 1);
    if (!valid) {
        writeStatus(statusIndex, false,
                    (index == 3)
                        ? QStringLiteral("pwm value %1 out of range (0-100)").arg(raw)
                        : QStringLiteral("value %1 out of range (0 or 1)").arg(raw));
        return;
    }
    if (m_canBus)
        m_canBus->setAux(index, raw);
    writeStatus(statusIndex, true);
    pushAuxState();
}

void BluetoothManager::pushAuxState()
{
    if (!m_auxService)
        return;
    QLowEnergyCharacteristic characteristic = m_auxService->characteristic(kAuxStateCharUuid);
    if (!characteristic.isValid())
        return;
    QByteArray state(4, char(0));
    for (int i = 0; i < 4; ++i)
        state[i] = char(m_canBus ? m_canBus->auxState(i) : 0);
    m_auxService->writeCharacteristic(characteristic, state);
}

void BluetoothManager::writeStatus(int index, bool ok, const QString &message)
{
    if (!m_auxService)
        return;
    QLowEnergyCharacteristic characteristic = m_auxService->characteristic(kStatusCharUuid);
    if (!characteristic.isValid())
        return;
    QString text = ok
        ? QStringLiteral("%1:OK").arg(index)
        : QStringLiteral("%1:ERROR:%2").arg(index).arg(message);
    m_auxService->writeCharacteristic(characteristic, text.toUtf8());
}

bool BluetoothManager::available() const
{
    return m_localDevice && m_localDevice->isValid();
}

QString BluetoothManager::localName() const
{
    return QString::fromLatin1(kLocalName);
}

void BluetoothManager::startAdvertising()
{
    ensureLocalDevice();
    if (!m_localDevice->isValid()) {
        // Boot-order race, hardware-confirmed 2026-08-19: main.cpp calls
        // this synchronously early in main(), which can beat bluetoothd
        // (Type=dbus, only "up" once its D-Bus name is registered — that
        // measured ~9s in vs. this call landing ~5-6s in on one real boot).
        // This didn't matter when advertising only started from a screen
        // tap long after boot had settled; moving it to boot exposed it.
        //
        // QBluetoothLocalDevice (Qt5 BlueZ backend) appears to snapshot
        // adapter presence via a one-shot D-Bus query made at construction
        // and does NOT notice bluetoothd starting afterward on its own —
        // confirmed empirically: retrying startAdvertising() while reusing
        // the same object (ensureLocalDevice()'s `if (m_localDevice)
        // return;` guard) left it permanently invalid across a full real
        // boot, `bluetoothctl show` meanwhile listing the controller fine
        // (`Powered: no` — bluetoothd knew about it, this object just never
        // asked again). So: delete and let the next ensureLocalDevice()
        // rebuild it from scratch, forcing a fresh query against a
        // (by-then, hopefully) running bluetoothd. Same "keep trying" shape
        // as CanBus::tryConnect() for the same class of startup race.
        fprintf(stderr, "[bluetooth] no adapter yet, retrying in 1s\n");
        delete m_localDevice;
        m_localDevice = nullptr;
        QTimer::singleShot(1000, this, &BluetoothManager::startAdvertising);
        return;
    }
    fprintf(stderr, "[bluetooth] adapter found, powering on and advertising\n");
    m_localDevice->powerOn();
    ensurePeripheralController();

    QLowEnergyAdvertisingData advertisingData;
    advertisingData.setDiscoverability(QLowEnergyAdvertisingData::DiscoverabilityGeneral);
    advertisingData.setLocalName(localName());

    QLowEnergyAdvertisingParameters params;
    m_peripheralController->startAdvertising(params, advertisingData);
    m_advertising = true;
    emit advertisingChanged();
}

void BluetoothManager::stopAdvertising()
{
    if (!m_peripheralController || !m_advertising)
        return;
    m_advertising = false;
    m_peripheralController->stopAdvertising();
    emit advertisingChanged();
}

void BluetoothManager::confirmPairing(bool accept)
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (!m_localDevice || m_pendingPairAddress.isEmpty() || !m_pendingPairNeedsConfirm)
        return;
    m_localDevice->pairingConfirmation(accept);
#else
    Q_UNUSED(accept);
#endif
}

void BluetoothManager::clearPendingPair()
{
    m_pendingPairAddress.clear();
    m_pendingPairName.clear();
    m_pendingPairCode.clear();
    m_pendingPairNeedsConfirm = false;
    emit pairingRequestChanged();
}
