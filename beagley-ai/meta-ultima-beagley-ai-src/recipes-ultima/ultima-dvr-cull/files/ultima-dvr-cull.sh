#!/bin/sh
# Free-space ring-buffer retention for the dashcam recordings (DASHCAM.md M2).
# When the DVR drive drops below MIN_FREE_PCT free, delete the oldest recordings
# first until it recovers — so the drive fills up, then rotates, adapting to any
# stick size with no per-drive configuration. Only ever touches *.h264 files
# under REC_DIR; a clean no-op when no drive is mounted. Deliberately a separate
# unit from ultima-app so neither can take the other down. Driven every few
# minutes by ultima-dvr-cull.timer.
set -eu

MNT=/mnt/dvr
REC_DIR="$MNT/ULTIMA"
MIN_FREE_PCT="${MIN_FREE_PCT:-10}"   # keep at least this % of the drive free

mountpoint -q "$MNT" 2>/dev/null || exit 0   # no drive plugged in -> nothing to do
[ -d "$REC_DIR" ] || exit 0                   # drive present but nothing recorded yet

# Percent of the drive currently free (100 - Use%). Plain df is single-line on
# busybox, so column 5 is Use% and NR==2 is the data row.
free_pct() { df "$MNT" 2>/dev/null | awk 'NR==2 { gsub("%","",$5); print 100 - $5 }'; }

fp=$(free_pct)
[ -n "${fp:-}" ] || exit 0
[ "$fp" -lt "$MIN_FREE_PCT" ] || exit 0       # already enough headroom

# Oldest-first. Segment paths are ULTIMA/<date>/<time>_cam.h264, so a plain
# lexical sort is chronological. (unsynced/ footage — written before the clock
# was set — sorts last and is thus kept longest; acceptable, and rare once the
# RTC is brought up in M3.) The app only ever holds the newest file open, so
# deleting from the oldest end never races the recorder.
find "$REC_DIR" -type f -name '*.h264' 2>/dev/null | sort | while IFS= read -r f; do
    cur=$(free_pct)
    { [ -n "${cur:-}" ] && [ "$cur" -lt "$MIN_FREE_PCT" ]; } || break
    rm -f "$f"
done

# Reap emptied day directories (rmdir refuses non-empty ones, so this is safe).
find "$REC_DIR" -type d 2>/dev/null | while IFS= read -r d; do
    [ "$d" = "$REC_DIR" ] || rmdir "$d" 2>/dev/null || true
done

echo "ultima-dvr-cull: DVR now $(free_pct)% free (target >= ${MIN_FREE_PCT}%)"
