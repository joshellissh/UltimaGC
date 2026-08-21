#ifndef SYSTEMCLOCK_H
#define SYSTEMCLOCK_H

#include <QObject>

// Lets the QML time-set screen push a new wall-clock time to the kernel.
// The app runs as root (its systemd unit has no User=), so clock_settime()
// needs no privilege escalation on target.
//
// Also best-effort writes through to a battery-backed hardware RTC at
// /dev/rtc0 if one is present, so the new time survives a reboot. BeaglePlay
// has one (onboard BQ32002). Probes for /dev/rtc0 at runtime and silently
// skips the write when it's absent — same pattern CanBus uses for can0.
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

    // False during the brief post-boot window where the system clock still
    // holds its stale boot-default value rather than the BQ32002 RTC's real
    // time (see beagleplay-falcon/NOTES.md "Dash clock doesn't persist a
    // manual set" — ultima-hwclock-load.service hasn't won its race with
    // ultima-app yet). Detected by reading /dev/rtc0 directly and comparing
    // against the system clock — not by comparing against a build
    // timestamp, which seemed simpler but isn't reliable (a build
    // machine's own clock can drift; see systemclock.cpp). Always true on
    // non-Linux dev builds.
    Q_INVOKABLE bool timeIsValid() const;
};

#endif
