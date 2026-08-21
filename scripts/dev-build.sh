#!/bin/sh
# --boot: simulate the on-target boot splash (see beagleplay-falcon/NOTES.md
# "Boot splash implemented") ahead of the app's own startup self-test sweep,
# so the whole boot flow can be screen-recorded in one shot without hardware.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$PROJECT_DIR/ultima-app"
BUILD_DIR="$PROJECT_DIR/build"

QMAKE="/opt/homebrew/opt/qt/bin/qmake6"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
"$QMAKE" "$SRC_DIR/ultima-app.pro"
make -j$(sysctl -n hw.ncpu)

if [ "$1" = "--boot" ]; then
    export ULTIMA_SPLASH_IMAGE="$PROJECT_DIR/splash screen.png"
fi

open ./ultima-app.app 2>/dev/null || ./ultima-app
