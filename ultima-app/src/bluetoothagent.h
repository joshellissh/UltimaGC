#ifndef BLUETOOTHAGENT_H
#define BLUETOOTHAGENT_H

// Q_OS_LINUX isn't a raw compiler predefine — it only exists once some Qt
// header defines it from the real predefines (__linux__ etc). Needed
// unconditionally, before the #ifdef below: without it, a translation unit
// whose *first* include is this header (this file's own .cpp does exactly
// that) evaluates Q_OS_LINUX as undefined even on Linux, silently
// compiling this whole file to nothing — confirmed the hard way
// (2026-08-20): moc reported "No relevant classes found" for this header,
// and the real compile produced an empty translation unit, leaving
// BluetoothAgent's constructor with a declaration (visible via
// bluetoothmanager.h's forward declaration + this header, when reached
// *after* something else had already pulled in a Qt header) but no
// definition anywhere — an "undefined reference" at link time, not a
// compile error, which is what made it non-obvious. <QtGlobal> alone did
// NOT fix this (tried first, same failure) — moc apparently doesn't fully
// walk it the way it does <QObject>, which bluetoothmanager.h's own
// (working) Q_OS_LINUX guards rely on already. Match that proven pattern
// exactly rather than the theoretically-more-minimal header.
#include <QObject>

// Whole file is Linux-only — resolves to an empty translation unit
// everywhere else (see the matching #ifdef in bluetoothagent.cpp), same
// approach as ultima-app.pro's `contains(QMAKE_PLATFORM, linux): QT +=
// dbus` rather than adding a second place that needs to know which
// platforms have QtDBus.
#ifdef Q_OS_LINUX

#include <QDBusAbstractAdaptor>
#include <QDBusContext>
#include <QDBusObjectPath>

class BluetoothManager;

// Implements org.bluez.Agent1 (see bluez's doc/org.bluez.Agent.rst) so
// bluetoothd actually has someone to call into during a pairing attempt.
//
// Without this registered, BlueZ cannot complete ANY pairing procedure —
// confirmed the hard way on real hardware (2026-08-20): every attempt
// failed with "No agent available for request type 2" /
// "device_confirm_passkey: Operation not permitted" in bluetoothd's own
// log, regardless of BluetoothManager's Pairable state. The class comment
// in bluetoothmanager.h used to assume QBluetoothLocalDevice's Qt5/BlueZ
// backend registered a working agent internally (that's what its
// pairingDisplayConfirmation/pairingDisplayPinCode signals implied) — it
// doesn't, at least not usefully, on this Qt 5.15.13 / BlueZ 5.72 pairing.
// This class replaces that assumption with an explicit registration
// (BluetoothManager::registerAgent()), and BluetoothManager's confirmPairing
// et al. now reply to the D-Bus calls captured here instead of calling into
// QBluetoothLocalDevice at all.
//
// Registered with IO capability "DisplayYesNo" — this dash has a screen and
// can show yes/no, but no keyboard, so RequestPinCode/RequestPasskey (which
// ask *us* to supply a secret) should never actually be invoked by BlueZ's
// own association-model selection given that capability; implemented
// defensively below (reject) anyway rather than left to return a bogus
// default if some peer combination reaches them regardless.
class BluetoothAgent : public QDBusAbstractAdaptor, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.bluez.Agent1")

public:
    explicit BluetoothAgent(BluetoothManager *manager);

public slots:
    void Release();
    QString RequestPinCode(const QDBusObjectPath &device);
    void DisplayPinCode(const QDBusObjectPath &device, const QString &pincode);
    quint32 RequestPasskey(const QDBusObjectPath &device);
    void DisplayPasskey(const QDBusObjectPath &device, quint32 passkey, quint16 entered);
    // The main path for this dash: numeric-comparison pairing. Delays the
    // D-Bus reply (setDelayedReply(true)) and hands the call context to
    // BluetoothManager, which sends the actual reply once the driver taps
    // Confirm/Reject on BluetoothScreen.qml — see
    // BluetoothManager::requestPairingConfirmation()/confirmPairing().
    void RequestConfirmation(const QDBusObjectPath &device, quint32 passkey);
    void RequestAuthorization(const QDBusObjectPath &device);
    void AuthorizeService(const QDBusObjectPath &device, const QString &uuid);
    void Cancel();

private:
    BluetoothManager *m_manager;
};

#endif // Q_OS_LINUX

#endif // BLUETOOTHAGENT_H
