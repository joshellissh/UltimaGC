#include "systemclock.h"

#include <QDateTime>
#include <QTime>
#include <stdio.h>

#ifdef __linux__
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#endif

SystemClock::SystemClock(QObject *parent) : QObject(parent)
{
}

#ifdef __linux__
// Best-effort — silently does nothing if there's no /dev/rtc0.
static void writeHardwareRtc(time_t t)
{
    int fd = open("/dev/rtc0", O_RDONLY);
    if (fd < 0)
        return;

    struct tm utc;
    gmtime_r(&t, &utc);

    struct rtc_time rtc = {};
    rtc.tm_sec = utc.tm_sec;
    rtc.tm_min = utc.tm_min;
    rtc.tm_hour = utc.tm_hour;
    rtc.tm_mday = utc.tm_mday;
    rtc.tm_mon = utc.tm_mon;
    rtc.tm_year = utc.tm_year;
    rtc.tm_wday = utc.tm_wday;
    rtc.tm_yday = utc.tm_yday;
    rtc.tm_isdst = utc.tm_isdst;

    if (ioctl(fd, RTC_SET_TIME, &rtc) != 0)
        fprintf(stderr, "SystemClock::setTime: RTC_SET_TIME failed: %s\n", strerror(errno));
    close(fd);
}
#endif

bool SystemClock::setTime(int hour, int minute)
{
#ifdef __linux__
    QDateTime target = QDateTime::currentDateTime();
    target.setTime(QTime(hour, minute, 0));

    time_t epochSecs = target.toSecsSinceEpoch();
    struct timespec ts;
    ts.tv_sec = epochSecs;
    ts.tv_nsec = 0;

    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        fprintf(stderr, "SystemClock::setTime: clock_settime failed: %s\n", strerror(errno));
        return false;
    }

    writeHardwareRtc(epochSecs);
    return true;
#else
    // Dev build on macOS: never touch the host machine's clock.
    Q_UNUSED(hour);
    Q_UNUSED(minute);
    fprintf(stderr, "SystemClock::setTime: no-op on non-Linux dev build\n");
    return false;
#endif
}

bool SystemClock::timeIsValid() const
{
#ifdef __linux__
    // Same signal ultima-hwclock-load.service itself waits on (see
    // beagleplay-falcon/NOTES.md "Dash clock doesn't persist a manual
    // set"): the system clock is still the kernel's stale boot-default
    // until that unit runs `hwclock --hctosys`. First attempt at this
    // compared against this binary's own compile-time build timestamp
    // instead — broke the moment the Docker build container's clock
    // turned out to be hours off (a real, observed OrbStack VM clock-drift
    // case), which made the point that no build machine's clock is
    // trustworthy ground truth either. Reading the RTC directly sidesteps
    // that: it's the same clock hwclock-load itself trusts.
    //
    // Fails closed (reports invalid) whenever /dev/rtc0 isn't there yet or
    // can't be read — including the brief window right after boot where
    // the app is already running but the bq32k driver hasn't registered
    // rtc0 yet, which is exactly the case this exists to catch.
    int fd = open("/dev/rtc0", O_RDONLY);
    if (fd < 0)
        return false;

    struct rtc_time rtc = {};
    bool ok = ioctl(fd, RTC_RD_TIME, &rtc) == 0;
    close(fd);
    if (!ok)
        return false;

    struct tm utc = {};
    utc.tm_sec = rtc.tm_sec;
    utc.tm_min = rtc.tm_min;
    utc.tm_hour = rtc.tm_hour;
    utc.tm_mday = rtc.tm_mday;
    utc.tm_mon = rtc.tm_mon;
    utc.tm_year = rtc.tm_year;
    time_t rtcEpoch = timegm(&utc);

    // Small slack for read/rounding skew between the two clocks, not a
    // real correctness boundary.
    return time(nullptr) >= rtcEpoch - 60;
#else
    return true;
#endif
}
