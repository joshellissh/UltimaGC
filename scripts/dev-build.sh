#!/bin/sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$PROJECT_DIR/br2-external/package/ultima-app/src"
BUILD_DIR="$PROJECT_DIR/build"

QMAKE="/opt/homebrew/opt/qt/bin/qmake6"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
"$QMAKE" "$SRC_DIR/ultima-app.pro"
make -j$(sysctl -n hw.ncpu)
open ./ultima-app.app 2>/dev/null || ./ultima-app
