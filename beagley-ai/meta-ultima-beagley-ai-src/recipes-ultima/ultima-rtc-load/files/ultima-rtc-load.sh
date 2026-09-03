#!/bin/sh
# Load the system clock from the onboard DS1340 RTC (rtc0) at boot, so the
# dashcam's segment filenames and the dash clock get a real date instead of the
# systemd build-epoch floor. rtc0 is the battery-backed DS1340 (kernel name
# "rtc-ds1307 2-0068"); rtc1 is the SoC RTC, which has an inaccurate ~32 kHz
# clock and no backup on this board, so it is deliberately not used.
#
# A clean no-op unless the RTC holds a plausible time: the DS1340 needs a coin
# cell on its backup connector AND a one-time set (see DASHCAM.md M3). Until
# then its oscillator is stopped, hwclock fails, and the clock is left as-is
# (ultima-app's SystemClock already shows --:-- while the time is unknown).
set -u

RTC=/dev/rtc0
[ -e "$RTC" ] || { logger -t ultima-rtc "no $RTC present; leaving clock as-is"; exit 0; }

# Short options only (-u UTC, -s hctosys, -f device) so this works with either
# busybox or util-linux hwclock. Failure = the RTC time is invalid (oscillator
# stopped: no battery / never set), which is a clean no-op, not a unit failure.
if hwclock -u -s -f "$RTC" 2>/dev/null; then
    logger -t ultima-rtc "system clock set from DS1340: $(date -u)"
else
    logger -t ultima-rtc "DS1340 time invalid (needs coin cell + one-time set); clock left as-is"
fi
exit 0
