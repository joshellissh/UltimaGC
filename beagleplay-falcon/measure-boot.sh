#!/usr/bin/env bash
# Measure BeaglePlay boot timing over serial with real per-line elapsed
# timestamps, instead of eyeballing a raw screenlog.0 (which is how
# NOTES.md's existing ~0.49s ATF->kernel and ~12.3s framebuffer numbers
# were produced).
#
# t=0 is the first byte received after this script arms the port — same
# caveat as those existing numbers: not a hardware-verified power-on zero,
# but tight, since the boot ROM starts writing to UART within tens of ms of
# reset. Only power on (or reset) the board AFTER the "Capture armed"
# prompt, never before.
#
# Auto-stops a few seconds after the framebuffer-ready landmark, or at
# max_seconds regardless (covers a boot that never gets that far).
#
# Usage:
#   ./measure-boot.sh [max_seconds]      default 60

set -euo pipefail
cd "$(dirname "$0")"

DEV=/dev/cu.usbserial-0001
MAX_DURATION="${1:-60}"
GRACE=5

command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 1; }
[ -e "$DEV" ] || { echo "No serial adapter at $DEV — is it plugged in?" >&2; exit 1; }

# Stale readers silently steal bytes (NOTES.md "Serial console caveats") —
# clear them before arming a new capture.
for p in $(lsof -t "$DEV" 2>/dev/null || true); do kill -9 "$p" 2>/dev/null || true; done

mkdir -p boot-logs
OUT="boot-logs/boot-$(date +%Y%m%dT%H%M%S).log"

PYSCRIPT="$(mktemp -t measure-boot.XXXXXX.py)"
cleanup() { rm -f "$PYSCRIPT"; exec 3<&- 2>/dev/null || true; }
trap cleanup EXIT

cat > "$PYSCRIPT" <<'PYEOF'
import sys, os, re, select, time

max_duration, grace, outpath = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3]

# Same landmark strings NOTES.md already uses to describe these boots, so a
# capture here reads as a direct, sourced replacement for the by-hand numbers.
LANDMARKS = [
    ("falcon payload load", re.compile(r"Loading falcon payload")),
    ("ATF start",           re.compile(r"Starting ATF on ARM64 core")),
    ("kernel entry",        re.compile(r"Linux version")),
    ("framebuffer ready",   re.compile(r"tidssdrmfb frame buffer device|Initialized tidss")),
]
STOP_LANDMARK = "framebuffer ready"

fd = 0  # stdin, redirected by the wrapper to the open serial fd
armed_at = time.time()
t0 = None
hit = {}
buf = b""
out = open(outpath, "w", buffering=1)

def emit(elapsed, line):
    out.write(f"{elapsed:8.3f}  {line}\n")
    print(f"{elapsed:8.3f}  {line}", flush=True)
    for name, pat in LANDMARKS:
        if name not in hit and pat.search(line):
            hit[name] = elapsed

while True:
    now = time.time()
    if t0 is None and now - armed_at > max_duration:
        print(f"\nNo serial data in {max_duration:.0f}s — board off, or wrong port?", file=sys.stderr)
        sys.exit(1)
    if t0 is not None:
        if now - t0 > max_duration:
            print(f"\n[stopping: reached max duration {max_duration:.0f}s]")
            break
        if STOP_LANDMARK in hit and now - t0 > hit[STOP_LANDMARK] + grace:
            print(f"\n[stopping: {grace:.0f}s past framebuffer-ready]")
            break

    ready, _, _ = select.select([fd], [], [], 0.5)
    if not ready:
        continue
    chunk = os.read(fd, 4096)
    if not chunk:
        break  # real EOF: port closed out from under us
    if t0 is None:
        t0 = time.time()
        emit(0.0, "-- first byte received, t=0 (power-on proxy) --")
    buf += chunk
    while b"\n" in buf:
        raw, buf = buf.split(b"\n", 1)
        emit(time.time() - t0, raw.decode("utf-8", errors="replace").rstrip("\r"))

out.close()

print("\n=== Landmarks (elapsed seconds since first byte) ===")
prev = 0.0
for name, _ in LANDMARKS:
    t = hit.get(name)
    if t is None:
        print(f"  {name:22s} not seen")
    else:
        print(f"  {name:22s} {t:7.3f}s   (+{t - prev:.3f}s)")
        prev = t
PYEOF

echo "Capture armed on $DEV. Power on (or reset) the board NOW."
echo "Auto-stops ${GRACE}s after framebuffer-ready, or after ${MAX_DURATION}s regardless."
echo

exec 3<>"$DEV"
stty -f /dev/fd/3 115200 cs8 -cstopb -parenb raw -echo

python3 "$PYSCRIPT" "$MAX_DURATION" "$GRACE" "$OUT" <&3

echo
echo "Full timestamped log: $OUT"
