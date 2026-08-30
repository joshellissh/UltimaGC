#ifndef SYSTEMCLOCK_H
#define SYSTEMCLOCK_H

#include <QObject>

// Lets the QML time-set screen push a new wall-clock time to the kernel.
// The app runs as root (its systemd unit has no User=), so clock_settime()
// needs no privilege escalation on target.
//
// Also best-effort writes through to a battery-backed hardware RTC at
// /dev/rtc0 if one is present, so the new time survives a reboot on boards
// that have one. Probes for /dev/rtc0 at runtime and silently
// skips the write when it's absent — same pattern CanBus uses for can0.
class SystemClock : public QObject
{
    Q_OBJECT

public:
    explicit SystemClock(QObject *parent = nullptr);

    // Sets the given calendar date and wall-clock time (seconds zeroed).
    // month is 1-12. Returns false if the date is invalid (e.g. Feb 30),
    // the underlying clock_settime() call fails (e.g. no permission), or,
    // on non-Linux dev builds, always — the host machine's clock is never
    // touched.
    Q_INVOKABLE bool setTime(int year, int month, int day, int hour, int minute);

    // False during the brief post-boot window where the system clock still
    // holds its stale boot-default value rather than the RTC's real time
    // (see GAUGE-CLUSTER.md "Dash clock doesn't persist a manual set" —
    // an RTC-to-system-clock load hasn't run yet). Detected by reading
    // /dev/rtc0 directly and comparing
    // against the system clock — not by comparing against a build
    // timestamp, which seemed simpler but isn't reliable (a build
    // machine's own clock can drift; see systemclock.cpp). Always true on
    // non-Linux dev builds.
    Q_INVOKABLE bool timeIsValid() const;
};

#endif
