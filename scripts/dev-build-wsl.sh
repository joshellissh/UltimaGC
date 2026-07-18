#!/bin/sh
# Local dev build/run for WSL2 + WSLg (Windows equivalent of dev-build.sh).
# Run from inside WSL Ubuntu, e.g.: wsl.exe -e ./scripts/dev-build-wsl.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$PROJECT_DIR/br2-external/package/ultima-app/src"

# Build outside the Windows-mounted drive (/mnt/...) — much faster than
# compiling through 9p, and avoids file-lock weirdness with Windows tools.
BUILD_DIR="$HOME/.cache/ultima-app-build"

QT_DIR="$(ls -d "$HOME"/Qt/*/gcc_64 2>/dev/null | sort -V | tail -1)"
if [ -z "$QT_DIR" ]; then
  echo "No Qt6 install found under ~/Qt. Install one with:" >&2
  echo "  pip3 install --user aqtinstall" >&2
  echo "  python3 -m aqt install-qt linux desktop 6.5.3 gcc_64" >&2
  exit 1
fi
QMAKE="$QT_DIR/bin/qmake6"

if ! dpkg -s libxcb-cursor0 >/dev/null 2>&1; then
  echo "Missing libxcb-cursor0 (required by Qt6's xcb platform plugin). Install with:" >&2
  echo "  sudo apt-get install -y libxcb-cursor0" >&2
  exit 1
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
"$QMAKE" "$SRC_DIR/ultima-app.pro" "CONFIG+=ultima_dev_sim"
make -j"$(nproc)"

LD_LIBRARY_PATH="$QT_DIR/lib" QT_QPA_PLATFORM=xcb ./ultima-app
