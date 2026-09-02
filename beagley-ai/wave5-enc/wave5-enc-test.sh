#!/bin/sh
# Wave5 encoder throughput test (dashcam M0). Captures raw UYVY off the camera,
# then encodes it through the hardware Wave5 H.264 encoder via the wave5enc
# harness (v4l2-ctl and stock GStreamer can't drive this encoder — see
# README.md), timing single- vs four-stream 1080p25. In-image tooling plus the
# cross-built wave5enc binary; nothing else compiled on the board.
#
# Overrides (env):
#   WAVE5ENC=/tmp/wave5enc   the encoder harness binary (cross-built, scp'd)
#   CAP=/dev/videoN          CSI capture node (default: auto by UYVY 1080p)
#   NFRAMES=125              raw frames to capture (~5s; 1 UYVY frame = 4.15MB)
#   ENCFRAMES=250            frames to encode per stream (loops the raw file)
#   WORKDIR=/path            scratch (default /mnt/dvr/wave5-m0 if mounted, else /tmp/wave5-m0)
#   BITRATE=8000000  GOP=25  W=1920  H=1080
# Flags:  --reuse   keep an existing raw capture instead of re-grabbing
set -eu

W="${W:-1920}"; H="${H:-1080}"
NFRAMES="${NFRAMES:-125}"; ENCFRAMES="${ENCFRAMES:-250}"
BITRATE="${BITRATE:-8000000}"; GOP="${GOP:-25}"
WAVE5ENC="${WAVE5ENC:-/tmp/wave5enc}"
REUSE=0; [ "${1:-}" = "--reuse" ] && REUSE=1

log() { echo "wave5-m0: $*"; }
die() { echo "wave5-m0: ERROR: $*" >&2; exit 1; }

[ -x "$WAVE5ENC" ] || die "$WAVE5ENC missing/not executable (scp the cross-built binary first)"
command -v v4l2-ctl >/dev/null 2>&1 || die "v4l2-ctl not found"

# VPU must be clean (refcount 0). A leaked instance means a prior wedge — a
# cold power-cycle is the only recovery (see README).
refcnt=$(awk '/^wave5 /{print $3}' /proc/modules 2>/dev/null || echo "?")
[ "$refcnt" = 0 ] || log "WARNING: wave5 module refcount=$refcnt (expected 0 — VPU may be wedged; cold power-cycle if this fails)"

if [ -z "${WORKDIR:-}" ]; then
    if mountpoint -q /mnt/dvr 2>/dev/null; then WORKDIR=/mnt/dvr/wave5-m0; else WORKDIR=/tmp/wave5-m0; fi
fi
mkdir -p "$WORKDIR"
RAW="$WORKDIR/cap_${W}x${H}_uyvy.raw"
log "workdir $WORKDIR"

# --- capture raw UYVY off the CSI camera (VC0) --------------------------------
is_capture() { v4l2-ctl -d "$1" --info 2>/dev/null | grep -qi "Video Capture" \
                 && ! v4l2-ctl -d "$1" --info 2>/dev/null | grep -qi "Memory-to-Memory"; }
capture_sample() {
    cap="$1"
    v4l2-ctl -d "$cap" --set-fmt-video="width=$W,height=$H,pixelformat=UYVY" >/dev/null 2>&1 || return 1
    v4l2-ctl -d "$cap" --get-fmt-video 2>/dev/null | grep -qi "UYVY" || return 1
    log "capturing $NFRAMES frames from $cap"
    v4l2-ctl -d "$cap" --set-fmt-video="width=$W,height=$H,pixelformat=UYVY" \
        --stream-mmap --stream-count="$NFRAMES" --stream-to="$RAW" >/dev/null 2>&1 || return 1
    want=$(( NFRAMES * W * H * 2 )); got=$(wc -c < "$RAW" 2>/dev/null || echo 0)
    [ "$got" -ge "$want" ]
}

if [ "$REUSE" = 1 ] && [ -s "$RAW" ]; then
    log "reusing $RAW ($(wc -c < "$RAW") bytes)"
elif [ -n "${CAP:-}" ]; then
    capture_sample "$CAP" || die "capture from $CAP failed (EPIPE? csi-setup not run yet; or cold power-cycle)"
    log "capture node: $CAP"
else
    CAP=""
    for n in /dev/video*; do
        [ -e "$n" ] || continue; is_capture "$n" || continue
        if capture_sample "$n"; then CAP="$n"; break; fi
    done
    [ -n "$CAP" ] || die "no capture node yielded UYVY ${W}x${H} — set CAP=/dev/videoN (VC0 is the lowest CSI ctx)"
    log "capture node: $CAP"
fi

# --- single-stream encode -----------------------------------------------------
log "--- single stream ($ENCFRAMES frames) ---"
"$WAVE5ENC" "$RAW" "$W" "$H" "$WORKDIR/enc_single.h264" "$ENCFRAMES" "$BITRATE" "$GOP"
sz=$(wc -c < "$WORKDIR/enc_single.h264"); [ "$sz" -gt 0 ] || die "single encode produced 0 bytes"
log "single output $sz bytes -> $WORKDIR/enc_single.h264"

# --- 4x concurrent (the real 4-camera question) -------------------------------
log "--- 4x concurrent ($ENCFRAMES frames each) ---"
u0=$(cut -d' ' -f1 /proc/uptime)
i=1; while [ "$i" -le 4 ]; do
    "$WAVE5ENC" "$RAW" "$W" "$H" "$WORKDIR/enc_x4_$i.h264" "$ENCFRAMES" "$BITRATE" "$GOP" >"$WORKDIR/log$i" 2>&1 &
    i=$((i+1))
done
wait
u1=$(cut -d' ' -f1 /proc/uptime)
for i in 1 2 3 4; do grep RESULT "$WORKDIR/log$i" 2>/dev/null | sed "s/^/  x4[$i] /"; done
awk -v a="$u0" -v b="$u1" -v f="$ENCFRAMES" 'BEGIN{e=b-a; if(e>0) printf "wave5-m0: 4x AGGREGATE %.1f fps over %.2fs (%d frames total)\n", (4*f)/e, e, 4*f}'
log "PASS if single >= 25 fps AND 4x aggregate >= 100 fps (four real-time 1080p streams)."
