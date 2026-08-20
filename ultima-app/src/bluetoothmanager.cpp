#include "bluetoothmanager.h"
#include "bluetoothagent.h"
#include "canbus.h"

#include <QTimer>
#include <QLowEnergyServiceData>
#include <QLowEnergyCharacteristicData>
#include <QLowEnergyDescriptorData>
#include <cstdio>

// QtDBus doesn't exist on macOS — see ultima-app.pro's
// `contains(QMAKE_PLATFORM, linux): QT += dbus`. Used for setAdapterPairable()
// and registerAgent()/the pairing-confirmation reply plumbing below; every
// call site already degrades to a no-op without it, matching how the rest
// of this class handles a missing adapter/controller.
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusArgument>
#endif

namespace {
// Reuses the name already chosen for the (since-reverted) WiFi AP — see
// beagleplay-falcon/NOTES.md "WiFi AP" — rather than invent a second brand
// name for the same car.
const char *const kLocalName = "Ultima RS";

// How long enterPairingMode() leaves the adapter bondable before it reverts
// to closed on its own if the driver never explicitly closes it — see
// BluetoothManager::enterPairingMode()'s header comment for what this
// window actually gates. 2 minutes: long enough to walk through a phone's
// own pairing flow without rushing, short enough that "forgot to close the
// screen" doesn't leave the car pairable for the rest of the drive.
constexpr int kPairingModeTimeoutMs = 120000;

// hci0 is assumed elsewhere in this codebase already (see NOTES.md's
// "Bluetooth via CC1352P7" hardware bring-up, which confirms it directly)
// as the only adapter this board ever has — not derived dynamically here
// for the same reason nothing else in this class derives it dynamically.
const char *const kAdapterObjectPath = "/org/bluez/hci0";

// Object path BluetoothAgent (see bluetoothagent.h) is registered at —
// under this app's own namespace, not BlueZ's, since it's this app's D-Bus
// object, not one BlueZ owns.
const char *const kAgentObjectPath = "/ultima/agent";

// BlueZ's Agent1 callbacks identify the remote device by D-Bus object path
// (e.g. "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"), not by the
// QBluetoothAddress-formatted string (colon-separated) m_connectedDeviceAddress
// already uses elsewhere in this class — converts the former to the latter
// so a pairing request can be compared/displayed consistently with whatever
// this class already knows about the currently-connected central. Not
// itself Q_OS_LINUX-gated (plain QString manipulation, no QtDBus types
// involved) even though every real caller is — showPairingCode() uses it
// too now, and it's simpler to leave this one helper universally available
// than to gate it and immediately need it unguarded anyway.
QString addressFromDevicePath(const QString &path)
{
    const QString marker = QStringLiteral("dev_");
    int idx = path.lastIndexOf(marker);
    if (idx < 0)
        return QString();
    return path.mid(idx + marker.size()).replace(QLatin1Char('_'), QLatin1Char(':'));
}

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
//
// AttAuthenticationRequired alongside AttEncryptionRequired (2026-08-20):
// encryption alone is satisfied by an unauthenticated "Just Works" bond —
// no code compared anywhere, so any phone in radio range that completes a
// bond could write here. Requiring authentication forces BlueZ to use an
// MITM-protected association method (numeric comparison / passkey) before
// a write will succeed, closing the gap ANDROID-BLE-INTEGRATION.md's
// "Connection & security model" section flagged as open. Unverified like
// the rest of this GATT path (see the class comment in
// bluetoothmanager.h): needs a hardware pass to confirm BlueZ actually
// negotiates an authenticated method rather than just failing every write,
// which depends on the IO capability the local agent registers with — see
// that same doc section for what to check.
QLowEnergyCharacteristicData makeAuxWriteChar(const QBluetoothUuid &uuid)
{
    QLowEnergyCharacteristicData data;
    data.setUuid(uuid);
    data.setProperties(QLowEnergyCharacteristic::Write);
    data.setValue(QByteArray(1, char(0)));
    data.setWriteConstraints(QBluetooth::AttAccessConstraint::AttEncryptionRequired
                              | QBluetooth::AttAccessConstraint::AttAuthenticationRequired);
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
    // Pairing-confirmation UI used to be wired through
    // QBluetoothLocalDevice::pairingDisplayConfirmation/pairingDisplayPinCode
    // here (Qt5-only — Qt6 dropped that API). Removed 2026-08-20: confirmed
    // on real hardware that constructing QBluetoothLocalDevice does not
    // actually register a working BlueZ Agent, so those signals never
    // fired — dead code creating false confidence, not a working feature.
    // See registerAgent()/BluetoothAgent (bluetoothagent.h) for the
    // replacement, and the class comment above for the failure that found
    // this.
    emit availableChanged();
}

void BluetoothManager::ensurePeripheralController()
{
    if (m_peripheralController)
        return;
    m_peripheralController = QLowEnergyController::createPeripheral(this);
    // QOverload needed on Qt5: QLowEnergyController has both a plain error()
    // getter and this error(Error) signal — &QLowEnergyController::error
    // alone is ambiguous between them (real compile error, not a style
    // nit: "no matching function... unresolved overloaded function type").
    // Qt6 renamed the signal to errorOccurred(Error), removing the clash
    // (and the old name entirely) — hit as a real build break against
    // Homebrew Qt 6.11.1 while testing this file's other changes
    // (2026-08-20), not assumed from changelogs. Qt5 — the actual Yocto
    // target toolchain — still only has the old name.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void (QLowEnergyController::*controllerErrorSignal)(QLowEnergyController::Error) =
        &QLowEnergyController::errorOccurred;
#else
    void (QLowEnergyController::*controllerErrorSignal)(QLowEnergyController::Error) =
        QOverload<QLowEnergyController::Error>::of(&QLowEnergyController::error);
#endif
    connect(m_peripheralController, controllerErrorSignal,
            this, [this](QLowEnergyController::Error error) {
        fprintf(stderr, "[bluetooth] controller error: %d\n", int(error));
        // Hardware-confirmed 2026-08-20: startAdvertising() below marks
        // m_advertising = true optimistically right after issuing the
        // request (QLowEnergyController's peripheral API doesn't offer a
        // synchronous success confirmation), and it silently failed once —
        // "qt.bluetooth.bluez: received advertising error" in the journal,
        // most likely the controller mid-connection-attempt with an
        // already-bonded phone trying to reconnect at the same moment.
        // Nothing else in this class notices or retries that specific
        // failure, so "Ultima RS" just permanently stops being visible
        // until the next restart — this is the fix, matching the same
        // "keep trying" shape startAdvertising() already uses for the
        // no-adapter-yet boot race.
        if (error == QLowEnergyController::AdvertisingError && m_advertising) {
            m_advertising = false;
            emit advertisingChanged();
            fprintf(stderr, "[bluetooth] advertising failed to start, retrying in 1s\n");
            QTimer::singleShot(1000, this, &BluetoothManager::startAdvertising);
        }
    });
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
    // Boot-order race, hardware-confirmed 2026-08-19: main.cpp calls this
    // synchronously early in main(), which can beat bluetoothd (Type=dbus,
    // only "up" once its D-Bus name is registered). This didn't matter
    // when advertising only started from a screen tap long after boot had
    // settled; moving it to boot exposed it.
    //
    // Root-caused 2026-08-20 via a ~30-reboot capture across three rounds
    // of instrumentation — the fix below is not the first thing tried; two
    // earlier, more surgical attempts were each empirically disproven on
    // real hardware before this one:
    //   1. Original theory: QBluetoothLocalDevice just needed a fresh
    //      object once bluetoothd was actually up, so ensureLocalDevice()
    //      deleted and recreated it every retry. Confirmed insufficient —
    //      a 10-reboot capture (2/10 hit the failure) showed a raw
    //      QDBusInterface probe against org.bluez.Adapter1 finding the
    //      adapter fine (correct address) on every retry while Powered
    //      stayed false forever and QBluetoothLocalDevice::allDevices()
    //      never recovered, even as the object was being recreated every
    //      second.
    //   2. Next theory: recreating the object wasn't the missing piece,
    //      powering the adapter was — this class's only powerOn() call was
    //      gated behind isValid() returning true, so if isValid() itself
    //      required Powered=true, that's a real deadlock. Fixed by having
    //      the retry path force Powered=true directly via D-Bus,
    //      independent of QBluetoothLocalDevice. Also empirically
    //      disproven: a second 10-reboot capture (5/10 hit it) showed the
    //      direct Powered=true write landing successfully — the very next
    //      probe each time confirmed powered=1 — yet
    //      QBluetoothLocalDevice::isValid() still never recovered, for the
    //      rest of every one of those boots.
    //   3. Actual conclusion: Powered state was never the blocker. What's
    //      actually happening is that bluetooth.service's D-Bus activation
    //      fails exactly once, early in boot ("Could not activate remote
    //      peer: unit failed" — almost certainly the kernel not having
    //      registered hci0 for the USB dongle yet at that instant), and
    //      Qt5's QtBluetooth BlueZ backend appears to poison some
    //      PROCESS-lifetime internal state on that first failed touch —
    //      not object-lifetime, since deleting/recreating
    //      QBluetoothLocalDevice every second across both experiments
    //      above never helped. A brand-new *process* started later in an
    //      already-stable boot always finds the adapter instantly; nothing
    //      this class can do to an already-poisoned process's
    //      QBluetoothLocalDevice ever recovers it.
    //
    // So: never let QBluetoothLocalDevice touch org.bluez for the first
    // time until an independent raw D-Bus probe (proven reliable across
    // every capture above — it recovers every single time, unlike
    // QBluetoothLocalDevice) has already confirmed the adapter exists and
    // is powered. Only probe/power via D-Bus before that point; construct
    // the first-ever QBluetoothLocalDevice (via ensureLocalDevice()) only
    // once it's safe to.
#ifdef Q_OS_LINUX
    if (!m_localDevice) {
        QDBusInterface probe(QStringLiteral("org.bluez"), QString::fromLatin1(kAdapterObjectPath),
                              QStringLiteral("org.bluez.Adapter1"), QDBusConnection::systemBus());
        if (!probe.isValid()) {
            fprintf(stderr, "[bluetooth] adapter not in BlueZ yet (%s), retrying in 1s\n",
                    qPrintable(probe.lastError().message()));
            QTimer::singleShot(1000, this, &BluetoothManager::startAdvertising);
            return;
        }
        if (!probe.property("Powered").toBool()) {
            fprintf(stderr, "[bluetooth] adapter %s found but unpowered, powering on "
                            "directly before ever touching QBluetoothLocalDevice\n",
                    qPrintable(probe.property("Address").toString()));
            if (!probe.setProperty("Powered", true)) {
                fprintf(stderr, "[bluetooth] direct Powered=true write failed (%s)\n",
                        qPrintable(probe.lastError().message()));
            }
            QTimer::singleShot(1000, this, &BluetoothManager::startAdvertising);
            return;
        }
    }
#endif
    ensureLocalDevice();
    if (!m_localDevice->isValid()) {
        // Shouldn't be reachable on Linux now that the guard above only
        // ever constructs this once the raw probe has confirmed the
        // adapter is present and powered — kept as a fallback (and as the
        // only path at all on non-Linux/no-QtDBus builds) rather than
        // assuming the guard covers every case that could ever exist.
        fprintf(stderr, "[bluetooth] no adapter yet, retrying in 1s\n");
        delete m_localDevice;
        m_localDevice = nullptr;
        QTimer::singleShot(1000, this, &BluetoothManager::startAdvertising);
        return;
    }
    fprintf(stderr, "[bluetooth] adapter found, powering on and advertising\n");
    m_localDevice->powerOn();
    // Re-asserts whatever the current intended state is, not unconditionally
    // false: this path also re-runs from the disconnected handler below
    // (restarting advertising after any central disconnects, including one
    // that has nothing to do with an in-progress pairing attempt), and
    // hardcoding false here would silently close an active pairing-mode
    // window out from under the driver — m_pairingModeActive would still
    // read true (UI unchanged) while the adapter quietly stopped accepting
    // the very bond the driver just opened this window for. An adapter's
    // Pairable state defaults to true on power-on unless told otherwise, so
    // this still has to run every time this path runs (including the first
    // time, when m_pairingModeActive is false) — just against the current
    // state, not a hardcoded one.
    setAdapterPairable(m_pairingModeActive);
    registerAgent();
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
#ifdef Q_OS_LINUX
    if (m_pendingPairAddress.isEmpty() || !m_pendingPairNeedsConfirm)
        return;
    // Replies to the D-Bus call BluetoothAgent::RequestConfirmation()
    // deferred (setDelayedReply(true)) — see requestPairingConfirmation()
    // below, which captured m_pendingPairConnection/m_pendingPairMessage
    // for exactly this. Not m_localDevice->pairingConfirmation(): that path
    // is gone (see ensureLocalDevice()'s comment) — it never actually
    // worked, since nothing was registering an Agent for it to answer on
    // behalf of.
    if (accept) {
        m_pendingPairConnection.send(m_pendingPairMessage.createReply());
    } else {
        m_pendingPairConnection.send(m_pendingPairMessage.createErrorReply(
            QStringLiteral("org.bluez.Error.Rejected"), QStringLiteral("rejected by driver")));
    }
    clearPendingPair();
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
#ifdef Q_OS_LINUX
    m_pendingPairConnection = QDBusConnection(QString());
    m_pendingPairMessage = QDBusMessage();
    stopWatchingDevicePaired();
#endif
    emit pairingRequestChanged();
    // Covers both outcomes cheaply: a completed pairing (accept, or the
    // Passkey Entry path via watchDevicePaired()) means a new bonded device
    // just showed up; a reject/cancel means nothing changed and this is just
    // a wasted D-Bus round trip, not a wrong one.
    refreshPairedDevices();
}

void BluetoothManager::enterPairingMode()
{
    setAdapterPairable(true);
    m_pairingModeActive = true;
    emit pairingModeChanged();

    if (!m_pairingModeTimer) {
        m_pairingModeTimer = new QTimer(this);
        m_pairingModeTimer->setSingleShot(true);
        connect(m_pairingModeTimer, &QTimer::timeout, this, &BluetoothManager::exitPairingMode);
    }
    m_pairingModeTimer->start(kPairingModeTimeoutMs);
}

void BluetoothManager::exitPairingMode()
{
    if (m_pairingModeTimer)
        m_pairingModeTimer->stop();
    setAdapterPairable(false);
    if (!m_pairingModeActive)
        return;
    m_pairingModeActive = false;
    emit pairingModeChanged();
}

void BluetoothManager::setAdapterPairable(bool pairable)
{
#ifdef Q_OS_LINUX
    // Talks to org.bluez.Adapter1 directly rather than through
    // QBluetoothLocalDevice, which doesn't expose Pairable at all (its
    // HostMode abstraction predates BLE peripheral-role use cases like this
    // one — see the class comment in bluetoothmanager.h). Deliberately an
    // adapter-level property rather than gating in the
    // pairingDisplayConfirmation/pairingDisplayPinCode handlers above:
    // BlueZ refuses a *new* bonding attempt outright while Pairable is
    // false, before any association method is even negotiated, so this
    // isn't defeated by a Just Works negotiation the way intercepting those
    // signals would be (Just Works has no display step to intercept — see
    // ANDROID-BLE-INTEGRATION.md's still-open IO-capability caveat). Does
    // NOT affect a device that's already bonded reconnecting — that reuses
    // the stored link key and never triggers a new pairing procedure at
    // all, independent of this flag.
    //
    // Unverified on real hardware like the rest of this file's newer
    // additions: confirm on the board that (a) Pairable=false actually
    // rejects a new bonding attempt from an unbonded phone rather than only
    // hiding it from bluetoothctl, and (b) an already-bonded phone still
    // reconnects fine while it's false.
    QDBusInterface adapter(QStringLiteral("org.bluez"), QString::fromLatin1(kAdapterObjectPath),
                           QStringLiteral("org.bluez.Adapter1"), QDBusConnection::systemBus());
    if (!adapter.isValid()) {
        fprintf(stderr, "[bluetooth] setAdapterPairable(%d): org.bluez.Adapter1 not reachable "
                        "over D-Bus (%s) — new-device pairing may be unintentionally open\n",
                pairable, qPrintable(adapter.lastError().message()));
        return;
    }
    if (!adapter.setProperty("Pairable", pairable)) {
        fprintf(stderr, "[bluetooth] setAdapterPairable(%d) failed (%s) — new-device pairing "
                        "may be unintentionally open\n",
                pairable, qPrintable(adapter.lastError().message()));
        return;
    }
    fprintf(stderr, "[bluetooth] adapter Pairable set to %s\n", pairable ? "true" : "false");
#else
    Q_UNUSED(pairable);
#endif
}

void BluetoothManager::refreshPairedDevices()
{
#ifdef Q_OS_LINUX
    QVariantList devices;
    // BlueZ has no "list bonded devices" call of its own — the standard way
    // (same thing `bluetoothctl devices Paired` and every other BlueZ D-Bus
    // client does) is ObjectManager.GetManagedObjects on the root path,
    // which dumps every object BlueZ currently exposes (adapters, every
    // device it has ever seen this boot — connected, bonded, or merely
    // discovered) as a nested oa{sa{sv}} dict, then filter for
    // org.bluez.Device1 objects under this adapter with Paired == true.
    QDBusInterface objectManager(QStringLiteral("org.bluez"), QStringLiteral("/"),
                                  QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                  QDBusConnection::systemBus());
    if (!objectManager.isValid()) {
        fprintf(stderr, "[bluetooth] refreshPairedDevices: ObjectManager not reachable (%s)\n",
                qPrintable(objectManager.lastError().message()));
        m_pairedDevices = devices;
        emit pairedDevicesChanged();
        return;
    }
    QDBusMessage reply = objectManager.call(QStringLiteral("GetManagedObjects"));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        fprintf(stderr, "[bluetooth] GetManagedObjects failed (%s)\n",
                qPrintable(reply.errorMessage()));
        m_pairedDevices = devices;
        emit pairedDevicesChanged();
        return;
    }

    // Manual QDBusArgument walk, not a QDBusReply<T> for some registered T:
    // the reply's single argument is `a{oa{sa{sv}}}` — object path -> (
    // interface name -> properties dict ). The innermost properties dict
    // (`a{sv}`) demarshals straight into QVariantMap via QtDBus's built-in
    // support for that exact signature; the two dict levels above it don't
    // have a built-in C++ type, so those are walked by hand. Every interface
    // entry's value is read into a QVariantMap even when it's an interface
    // this loop doesn't care about (anything but org.bluez.Device1) —
    // skipping the read instead of the result would desync the iterator.
    const QString adapterPrefix = QString::fromLatin1(kAdapterObjectPath) + QStringLiteral("/");
    QDBusArgument arg = reply.arguments().value(0).value<QDBusArgument>();
    arg.beginMap();
    while (!arg.atEnd()) {
        arg.beginMapEntry();
        QDBusObjectPath objectPath;
        arg >> objectPath;

        QVariantMap deviceProps;
        bool isDevice = false;
        arg.beginMap();
        while (!arg.atEnd()) {
            arg.beginMapEntry();
            QString interfaceName;
            QVariantMap props;
            arg >> interfaceName >> props;
            arg.endMapEntry();
            if (interfaceName == QStringLiteral("org.bluez.Device1")) {
                deviceProps = props;
                isDevice = true;
            }
        }
        arg.endMap();
        arg.endMapEntry();

        if (!isDevice || !objectPath.path().startsWith(adapterPrefix))
            continue;
        if (!deviceProps.value(QStringLiteral("Paired")).toBool())
            continue;

        QVariantMap device;
        const QString address = deviceProps.value(QStringLiteral("Address")).toString();
        device[QStringLiteral("address")] = address;
        device[QStringLiteral("name")] = deviceProps.value(QStringLiteral("Name"),
            deviceProps.value(QStringLiteral("Alias"), address)).toString();
        device[QStringLiteral("connected")] = deviceProps.value(QStringLiteral("Connected"), false).toBool();
        devices.append(device);
    }
    arg.endMap();

    if (devices != m_pairedDevices) {
        m_pairedDevices = devices;
        emit pairedDevicesChanged();
    }
#endif
}

void BluetoothManager::forgetDevice(const QString &address)
{
#ifdef Q_OS_LINUX
    if (address.isEmpty())
        return;
    // BlueZ device object paths are the adapter path plus "/dev_" and the
    // address with colons swapped for underscores — the exact inverse of
    // addressFromDevicePath() above, and same casing (BlueZ reports
    // addresses upper-case, matching what Address ended up as in
    // refreshPairedDevices()'s device map, so no case conversion needed).
    const QString devicePath = QString::fromLatin1(kAdapterObjectPath) + QStringLiteral("/dev_")
        + QString(address).replace(QLatin1Char(':'), QLatin1Char('_'));
    QDBusInterface adapter(QStringLiteral("org.bluez"), QString::fromLatin1(kAdapterObjectPath),
                           QStringLiteral("org.bluez.Adapter1"), QDBusConnection::systemBus());
    if (!adapter.isValid()) {
        fprintf(stderr, "[bluetooth] forgetDevice(%s): org.bluez.Adapter1 not reachable (%s)\n",
                qPrintable(address), qPrintable(adapter.lastError().message()));
        return;
    }
    // RemoveDevice both erases the bond and disconnects the device first if
    // it's currently connected — the existing QLowEnergyController::disconnected
    // handler (allAuxOff() + advertising restart) already covers the fallout
    // of that disconnect, nothing extra needed here for it.
    QDBusMessage reply = adapter.call(QStringLiteral("RemoveDevice"),
                                        QVariant::fromValue(QDBusObjectPath(devicePath)));
    if (reply.type() == QDBusMessage::ErrorMessage) {
        fprintf(stderr, "[bluetooth] RemoveDevice(%s) failed (%s)\n",
                qPrintable(address), qPrintable(reply.errorMessage()));
    } else {
        fprintf(stderr, "[bluetooth] forgot device %s\n", qPrintable(address));
    }
    refreshPairedDevices();
#else
    Q_UNUSED(address);
#endif
}

void BluetoothManager::registerAgent()
{
#ifdef Q_OS_LINUX
    if (m_agentRegistered)
        return;
    if (!m_agent)
        m_agent = new BluetoothAgent(this);
    if (!QDBusConnection::systemBus().registerObject(QString::fromLatin1(kAgentObjectPath), this,
                                                       QDBusConnection::ExportAdaptors)) {
        fprintf(stderr, "[bluetooth] failed to register agent D-Bus object at %s\n", kAgentObjectPath);
        return;
    }
    QDBusInterface agentManager(QStringLiteral("org.bluez"), QStringLiteral("/org/bluez"),
                                 QStringLiteral("org.bluez.AgentManager1"), QDBusConnection::systemBus());
    if (!agentManager.isValid()) {
        fprintf(stderr, "[bluetooth] org.bluez.AgentManager1 not reachable (%s)\n",
                qPrintable(agentManager.lastError().message()));
        return;
    }
    // DisplayYesNo: this dash has a screen and can show yes/no, but no
    // keyboard — see bluetoothagent.h's class comment. Drives BlueZ toward
    // Numeric Comparison (BluetoothAgent::RequestConfirmation(), the main
    // path this app's UI is built around) when the peer also supports
    // display+confirm, which essentially every modern phone does.
    QDBusMessage registerReply = agentManager.call(QStringLiteral("RegisterAgent"),
                                                     QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kAgentObjectPath))),
                                                     QStringLiteral("DisplayYesNo"));
    if (registerReply.type() == QDBusMessage::ErrorMessage) {
        fprintf(stderr, "[bluetooth] RegisterAgent failed (%s) — pairing will not work\n",
                qPrintable(registerReply.errorMessage()));
        return;
    }
    QDBusMessage defaultReply = agentManager.call(QStringLiteral("RequestDefaultAgent"),
                                                    QVariant::fromValue(QDBusObjectPath(QString::fromLatin1(kAgentObjectPath))));
    if (defaultReply.type() == QDBusMessage::ErrorMessage) {
        fprintf(stderr, "[bluetooth] RequestDefaultAgent failed (%s) — pairing will not work\n",
                qPrintable(defaultReply.errorMessage()));
        return;
    }
    m_agentRegistered = true;
    fprintf(stderr, "[bluetooth] pairing agent registered (DisplayYesNo)\n");
#endif
}

#ifdef Q_OS_LINUX
void BluetoothManager::requestPairingConfirmation(const QString &devicePath, quint32 passkey,
                                                   const QDBusConnection &connection, const QDBusMessage &message)
{
    m_pendingPairConnection = connection;
    m_pendingPairMessage = message;
    m_pendingPairAddress = addressFromDevicePath(devicePath);
    m_pendingPairName = m_connectedDeviceAddress == m_pendingPairAddress && !m_connectedDeviceName.isEmpty()
        ? m_connectedDeviceName : m_pendingPairAddress;
    // BlueZ's Numeric Comparison passkey is a 6-digit code, zero-padded —
    // matches what BluetoothScreen.qml expects in pendingPairCode.
    m_pendingPairCode = QStringLiteral("%1").arg(passkey, 6, 10, QLatin1Char('0'));
    m_pendingPairNeedsConfirm = true;
    emit pairingRequestChanged();
}
#endif

void BluetoothManager::showPairingCode(const QString &devicePath, const QString &code, bool needsConfirm)
{
    // Display-only (BluetoothAgent::DisplayPasskey()/DisplayPinCode()) —
    // no D-Bus reply owed back to BlueZ for these, unlike
    // requestPairingConfirmation() above, so no connection/message to
    // stash. Derive address/name from devicePath the same way
    // requestPairingConfirmation() does, now that BluetoothAgent actually
    // passes it through (2026-08-20 — it used to just guess from
    // m_connectedDeviceAddress, which happened to work since only one
    // central talks to this peripheral at a time, but there's no reason to
    // guess when the real path is right there).
    m_pendingPairAddress = addressFromDevicePath(devicePath);
    m_pendingPairName = m_connectedDeviceAddress == m_pendingPairAddress && !m_connectedDeviceName.isEmpty()
        ? m_connectedDeviceName : m_pendingPairAddress;
    m_pendingPairCode = code;
    m_pendingPairNeedsConfirm = needsConfirm;
#ifdef Q_OS_LINUX
    // This flow has no explicit agent-level "pairing finished" callback —
    // see watchDevicePaired()'s own comment for why this is the only
    // reliable way to notice and close this prompt.
    watchDevicePaired(devicePath);
#endif
    emit pairingRequestChanged();
}

void BluetoothManager::cancelPairingConfirmation()
{
#ifdef Q_OS_LINUX
    // If a RequestConfirmation reply was still outstanding when BlueZ
    // cancelled (peer disconnected, procedure timed out, etc.), reply now
    // rather than leaving that D-Bus call permanently unanswered.
    if (m_pendingPairNeedsConfirm && m_pendingPairConnection.isConnected()) {
        m_pendingPairConnection.send(m_pendingPairMessage.createErrorReply(
            QStringLiteral("org.bluez.Error.Canceled"), QStringLiteral("pairing cancelled")));
    }
#endif
    clearPendingPair();
}

#ifdef Q_OS_LINUX
void BluetoothManager::watchDevicePaired(const QString &devicePath)
{
    if (devicePath.isEmpty() || devicePath == m_watchedDevicePath)
        return;
    stopWatchingDevicePaired();
    bool connected = QDBusConnection::systemBus().connect(
        QStringLiteral("org.bluez"), devicePath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged"), this,
        SLOT(onDevicePropertiesChanged(QString,QVariantMap,QStringList)));
    if (!connected) {
        fprintf(stderr, "[bluetooth] failed to watch %s for Paired — this pairing prompt won't "
                        "auto-close even if pairing succeeds\n", qPrintable(devicePath));
        return;
    }
    m_watchedDevicePath = devicePath;
}

void BluetoothManager::stopWatchingDevicePaired()
{
    if (m_watchedDevicePath.isEmpty())
        return;
    QDBusConnection::systemBus().disconnect(
        QStringLiteral("org.bluez"), m_watchedDevicePath, QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged"), this,
        SLOT(onDevicePropertiesChanged(QString,QVariantMap,QStringList)));
    m_watchedDevicePath.clear();
}

void BluetoothManager::onDevicePropertiesChanged(const QString &interface, const QVariantMap &changed,
                                                  const QStringList &invalidated)
{
    Q_UNUSED(invalidated);
    if (interface != QStringLiteral("org.bluez.Device1"))
        return;
    if (changed.value(QStringLiteral("Paired")).toBool() || changed.value(QStringLiteral("Bonded")).toBool()) {
        fprintf(stderr, "[bluetooth] Device1.Paired confirmed via D-Bus — closing pairing prompt\n");
        clearPendingPair();
    }
}
#endif
