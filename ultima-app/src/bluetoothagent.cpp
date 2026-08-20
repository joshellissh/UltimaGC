#include "bluetoothagent.h"

#ifdef Q_OS_LINUX

#include "bluetoothmanager.h"

BluetoothAgent::BluetoothAgent(BluetoothManager *manager)
    : QDBusAbstractAdaptor(manager), m_manager(manager)
{
}

void BluetoothAgent::Release()
{
}

QString BluetoothAgent::RequestPinCode(const QDBusObjectPath &device)
{
    Q_UNUSED(device);
    // BR/EDR legacy PIN entry — this board's controller has no BR/EDR radio
    // at all (see bluetoothmanager.h's class comment), and DisplayYesNo
    // shouldn't lead BlueZ here for LE pairing. Reject rather than return
    // an empty/insecure default if it somehow is reached anyway.
    sendErrorReply(QStringLiteral("org.bluez.Error.Rejected"),
                   QStringLiteral("no keyboard input available"));
    return QString();
}

void BluetoothAgent::DisplayPinCode(const QDBusObjectPath &device, const QString &pincode)
{
    m_manager->showPairingCode(device.path(), pincode, /*needsConfirm=*/false);
}

quint32 BluetoothAgent::RequestPasskey(const QDBusObjectPath &device)
{
    Q_UNUSED(device);
    sendErrorReply(QStringLiteral("org.bluez.Error.Rejected"),
                   QStringLiteral("no keyboard input available"));
    return 0;
}

void BluetoothAgent::DisplayPasskey(const QDBusObjectPath &device, quint32 passkey, quint16 entered)
{
    Q_UNUSED(entered);
    // Passkey Entry, display-only side: the peer types this in, nothing to
    // confirm here. Called repeatedly as `entered` advances — just keep the
    // displayed code current, same as the one-shot case. Zero-padded to 6
    // digits, matching BluetoothManager::requestPairingConfirmation()'s
    // formatting and, more importantly, the actual 6-digit value the peer
    // is expected to type — QString::number(passkey) alone (the previous
    // code here) would show "42" instead of "000042" for a low passkey
    // value, which is simply the wrong code to type in, not just a display
    // nit.
    m_manager->showPairingCode(device.path(), QStringLiteral("%1").arg(passkey, 6, 10, QLatin1Char('0')),
                                /*needsConfirm=*/false);
}

void BluetoothAgent::RequestConfirmation(const QDBusObjectPath &device, quint32 passkey)
{
    setDelayedReply(true);
    m_manager->requestPairingConfirmation(device.path(), passkey, connection(), message());
}

void BluetoothAgent::RequestAuthorization(const QDBusObjectPath &device)
{
    Q_UNUSED(device);
    // Pairable being open at all (BluetoothManager::enterPairingMode()) is
    // the actual access-control decision for whether a new device gets this
    // far — if BlueZ reached this callback, the adapter already allowed the
    // attempt. Nothing further to gate per-device today; accept (return
    // normally = success for a void D-Bus method).
}

void BluetoothAgent::AuthorizeService(const QDBusObjectPath &device, const QString &uuid)
{
    Q_UNUSED(device);
    Q_UNUSED(uuid);
    // Same reasoning as RequestAuthorization above.
}

void BluetoothAgent::Cancel()
{
    m_manager->cancelPairingConfirmation();
}

#endif // Q_OS_LINUX
