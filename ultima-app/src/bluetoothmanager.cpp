#include "bluetoothmanager.h"

#include <QTimer>
#include <cstdio>

namespace {
// Reuses the name already chosen for the (since-reverted) WiFi AP — see
// beagleplay-falcon/NOTES.md "WiFi AP" — rather than invent a second brand
// name for the same car.
const char *const kLocalName = "Ultima RS";
}

BluetoothManager::BluetoothManager(QObject *parent)
    : QObject(parent)
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
    });
    connect(m_peripheralController, &QLowEnergyController::disconnected, this, [this]() {
        m_connectedDeviceAddress.clear();
        m_connectedDeviceName.clear();
        emit connectionChanged();
        // A central disconnecting doesn't stop advertising on its own —
        // restart it so the next phone can find and connect to the dash
        // without the driver having to reopen this screen.
        if (m_advertising)
            startAdvertising();
    });
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
