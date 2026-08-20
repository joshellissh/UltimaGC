#ifndef BLUETOOTHMANAGER_H
#define BLUETOOTHMANAGER_H

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QBluetoothLocalDevice>
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyAdvertisingData>
#include <QLowEnergyAdvertisingParameters>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyService>

// Only needed for requestPairingConfirmation()'s parameter types below (the
// deferred D-Bus reply BluetoothAgent hands off — see bluetoothagent.h).
// Linux-only like everything else QtDBus-related in this class.
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#endif

class QTimer;
class CanBus;
class BluetoothAgent;

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
// Pairing-confirmation handling used to be wired through
// QBluetoothLocalDevice's pairingDisplayConfirmation/pairingDisplayPinCode
// signals, on the assumption that constructing it registered a working
// BlueZ Agent internally. Confirmed wrong on real hardware (2026-08-20):
// every pairing attempt failed with "No agent available for request type
// 2" in bluetoothd's own log, regardless of adapter Pairable state — that
// assumption had simply never been exercised end-to-end before. Replaced
// with an explicit org.bluez.Agent1 registration (registerAgent(), see the
// .cpp) backed by BluetoothAgent (bluetoothagent.h) — confirmPairing() now
// replies to the D-Bus call BluetoothAgent hands off via
// requestPairingConfirmation(), not to QBluetoothLocalDevice.
//
// BeaglePlay's Bluetooth controller (the onboard CC1352P7, flashed with
// BLE HCI firmware, attached over UART) has no BR/EDR radio at all.
class BluetoothManager : public QObject
{
    Q_OBJECT
    // BluetoothAgent (bluetoothagent.h, Linux-only) calls the pairing-
    // confirmation bridge methods below (requestPairingConfirmation(),
    // showPairingCode(), cancelPairingConfirmation()) — private since
    // nothing else, QML included, should touch them directly, just not
    // private *from* the one class that's specifically their caller. Safe
    // on non-Linux builds too: the friend declaration is a no-op if
    // BluetoothAgent is never defined there.
    friend class BluetoothAgent;
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(bool advertising READ advertising NOTIFY advertisingChanged)
    Q_PROPERTY(QString connectedDeviceName READ connectedDeviceName NOTIFY connectionChanged)
    Q_PROPERTY(QString connectedDeviceAddress READ connectedDeviceAddress NOTIFY connectionChanged)
    Q_PROPERTY(QString localName READ localName CONSTANT)
    Q_PROPERTY(QString pendingPairAddress READ pendingPairAddress NOTIFY pairingRequestChanged)
    Q_PROPERTY(QString pendingPairName READ pendingPairName NOTIFY pairingRequestChanged)
    Q_PROPERTY(QString pendingPairCode READ pendingPairCode NOTIFY pairingRequestChanged)
    Q_PROPERTY(bool pendingPairNeedsConfirm READ pendingPairNeedsConfirm NOTIFY pairingRequestChanged)
    Q_PROPERTY(bool pairingModeActive READ pairingModeActive NOTIFY pairingModeChanged)
    // Bonded devices known to the adapter (not just the one currently
    // connected) — see refreshPairedDevices() below. Each entry is a
    // QVariantMap {address, name, connected}. Empty (and never populated) on
    // any build without QtDBus, same degrade-gracefully posture as the rest
    // of this class.
    Q_PROPERTY(QVariantList pairedDevices READ pairedDevices NOTIFY pairedDevicesChanged)

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
    bool pairingModeActive() const { return m_pairingModeActive; }
    QVariantList pairedDevices() const { return m_pairedDevices; }

    Q_INVOKABLE void startAdvertising();
    Q_INVOKABLE void stopAdvertising();
    // Answers a pending numeric-comparison pairing prompt (see
    // pendingPairNeedsConfirm above) — no-op if nothing is pending.
    Q_INVOKABLE void confirmPairing(bool accept);
    // Opens a timed window (see kPairingModeTimeoutMs in the .cpp) during
    // which the adapter accepts *new* bonds. Outside this window the
    // adapter is non-bondable — an unwanted device can still connect and
    // read the unauthenticated characteristics, but can't complete a new
    // bond at all, regardless of pairing/association method (this is an
    // adapter-level BlueZ property, not something layered on top of the
    // confirm-signal flow above, so it isn't defeated by a Just Works
    // negotiation the way gating that signal alone would be — see the .cpp
    // for why). A device that's already bonded reconnects freely at any
    // time, pairing mode active or not — reconnecting reuses the existing
    // bond and never goes through this at all.
    Q_INVOKABLE void enterPairingMode();
    // Closes the window early — called on BluetoothScreen close so leaving
    // the screen actually ends the exposure rather than waiting out
    // whatever's left of kPairingModeTimeoutMs in the background.
    Q_INVOKABLE void exitPairingMode();
    // Re-queries BlueZ for the adapter's bonded devices and updates
    // pairedDevices (emitting pairedDevicesChanged() if the list actually
    // changed) — see the .cpp for the GetManagedObjects D-Bus call this
    // does. Called from BluetoothScreen.qml's open() so the list is fresh
    // every time the driver looks at it, and internally after a pairing
    // completes or a device is forgotten. No-op (leaves pairedDevices
    // empty) on any build without QtDBus.
    Q_INVOKABLE void refreshPairedDevices();
    // Unpairs (org.bluez.Adapter1.RemoveDevice) the bonded device at
    // `address` (colon-separated, as returned in a pairedDevices entry).
    // BlueZ's RemoveDevice disconnects the device first if it's currently
    // connected — same physical effect as the failsafe path in
    // ensurePeripheralController()'s disconnected handler (allAuxOff(),
    // advertising restart), no extra wiring needed here for that. Refreshes
    // pairedDevices afterward. No-op on any build without QtDBus.
    Q_INVOKABLE void forgetDevice(const QString &address);

signals:
    void availableChanged();
    void advertisingChanged();
    void connectionChanged();
    void pairingRequestChanged();
    void pairingModeChanged();
    void pairedDevicesChanged();

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
    // Sets org.bluez.Adapter1's Pairable property over the system D-Bus
    // (QtDBus — Linux-only, see ultima-app.pro). No-op (logs and returns)
    // on any platform/build without it, or if the D-Bus call itself fails —
    // same degrade-gracefully posture as the rest of this class, though
    // unlike most of those cases a failure here is a security-relevant one:
    // it means pairing mode couldn't actually be locked down, not just that
    // a nice-to-have feature is unavailable. See enterPairingMode()'s
    // comment in the header for what this is actually defending against.
    void setAdapterPairable(bool pairable);
    // Registers BluetoothAgent as the system's default BlueZ pairing agent
    // (org.bluez.AgentManager1.RegisterAgent + RequestDefaultAgent, IO
    // capability "DisplayYesNo") — see bluetoothagent.h for why this exists
    // at all. Lazy/idempotent like ensureLocalDevice() et al.; same
    // degrade-gracefully-and-log posture as setAdapterPairable() on
    // failure or on a platform/build without QtDBus.
    void registerAgent();
    // Bridge methods BluetoothAgent (Linux-only) calls into. Declared
    // unconditionally for simplicity even though nothing calls them on a
    // platform without BluetoothAgent, rather than scattering #ifdef at
    // call sites — matches setAdapterPairable()'s own shape.
#ifdef Q_OS_LINUX
    // BluetoothAgent::RequestConfirmation() hands off its deferred D-Bus
    // reply here; confirmPairing() sends the actual reply once the driver
    // taps Confirm/Reject.
    void requestPairingConfirmation(const QString &devicePath, quint32 passkey,
                                     const QDBusConnection &connection, const QDBusMessage &message);
#endif
    // BluetoothAgent::DisplayPasskey()/DisplayPinCode() — display-only,
    // nothing to reply to BlueZ for these (see BluetoothAgent's own
    // comments), unlike requestPairingConfirmation() above. devicePath is
    // the BlueZ object path (e.g. "/org/bluez/hci0/dev_AA_BB_..."), used
    // both to derive m_pendingPairAddress and to watch for pairing
    // actually finishing — see watchDevicePaired() below.
    void showPairingCode(const QString &devicePath, const QString &code, bool needsConfirm);
    // BluetoothAgent::Cancel() — BlueZ aborted the pairing procedure
    // (peer disconnected, timeout, etc.); clears any pending prompt and,
    // if a RequestConfirmation reply was still outstanding, replies with
    // an error so nothing is left hanging D-Bus-side.
    void cancelPairingConfirmation();

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
#ifdef Q_OS_LINUX
    // Passkey-entry/PIN-display pairing (showPairingCode()'s callers) has
    // no explicit "you're done" callback on the agent interface — BlueZ
    // completes the procedure entirely on its own once the peer finishes
    // typing, so the only reliable way to notice is watching
    // org.bluez.Device1's own Paired property directly. Confirmed
    // necessary on real hardware (2026-08-20): a phone paired successfully
    // via this exact flow and QBluetoothLocalDevice::pairingFinished
    // (which used to be relied on for this) never fired, leaving the
    // pairing prompt stuck open indefinitely despite pairing having
    // actually succeeded. Idempotent — re-watching the same path is a
    // no-op; watching a new path first tears down any previous watch.
    void watchDevicePaired(const QString &devicePath);
    // Tears down whatever watchDevicePaired() set up, if anything. Called
    // from clearPendingPair() so the watch never outlives the prompt it
    // was backing, regardless of which path actually closed the prompt
    // (tap Confirm/Reject, BlueZ Cancel, or this watch firing itself).
    void stopWatchingDevicePaired();
#endif

#ifdef Q_OS_LINUX
    // watchDevicePaired()'s target — needs to be a real Qt slot (not a
    // plain method) since QDBusConnection::connect()'s string-based
    // overload dispatches through the meta-object system like any other
    // signal/slot connection. Filters to org.bluez.Device1's Paired (or
    // Bonded, belt and suspenders — BlueZ documents Paired as the
    // authoritative one, but nothing says both always change together)
    // becoming true.
private slots:
    void onDevicePropertiesChanged(const QString &interface, const QVariantMap &changed,
                                    const QStringList &invalidated);

private:
#endif
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
    bool m_pairingModeActive = false;
    // Populated by refreshPairedDevices() — see pairedDevices Q_PROPERTY.
    QVariantList m_pairedDevices;
    // Single-shot; (re)started by enterPairingMode(), fires exitPairingMode()
    // if the driver doesn't close the screen (and so call exitPairingMode()
    // directly) first. Lazily constructed like the adapter/controller
    // members above, for the same reason: a drive that never opens this
    // screen never touches any of it.
    QTimer *m_pairingModeTimer = nullptr;

    // Lazy, constructed the first time registerAgent() runs. Owned (parented
    // to `this`), same lifetime posture as the rest of this class's lazy
    // members.
    BluetoothAgent *m_agent = nullptr;
    bool m_agentRegistered = false;
#ifdef Q_OS_LINUX
    // Captured by requestPairingConfirmation() so confirmPairing() can send
    // the actual reply later, once the driver responds — see
    // BluetoothAgent::RequestConfirmation()'s setDelayedReply(true). Only
    // meaningful while m_pendingPairNeedsConfirm is true; the display-only
    // flows (showPairingCode()) never populate these since BlueZ doesn't
    // wait on a reply for those.
    QDBusConnection m_pendingPairConnection{QString()};
    QDBusMessage m_pendingPairMessage;
    // Empty when watchDevicePaired() isn't currently watching anything —
    // see that method and stopWatchingDevicePaired().
    QString m_watchedDevicePath;
#endif
};

#endif
