#ifndef SYSTEMCLOCK_H
#define SYSTEMCLOCK_H

#include <QObject>

// Lets the QML time-set screen push a new wall-clock time to the kernel.
// The app runs as root (see S11app), so clock_settime() needs no privilege
// escalation on target.
//
// Also best-effort writes through to a battery-backed hardware RTC at
// /dev/rtc0 if one is present, so the new time survives a reboot. BeaglePlay
// has one (onboard BQ32002); RPi5 doesn't. Rather than fork this class per
// board, it just probes for /dev/rtc0 at runtime and silently skips the
// write when it's absent — same pattern CanBus uses for can0.
class SystemClock : public QObject
{
    Q_OBJECT

public:
    explicit SystemClock(QObject *parent = nullptr);

    // Sets today's date at the given wall-clock time (seconds zeroed).
    // Returns false if the underlying clock_settime() call fails (e.g. no
    // permission) or, on non-Linux dev builds, always — the host machine's
    // clock is never touched.
    Q_INVOKABLE bool setTime(int hour, int minute);
};

#endif
