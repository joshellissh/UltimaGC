#!/usr/bin/env bash
# Build tisdk-base-image for beagleplay-ti with ultima-app + Qt5 (linuxfb) +
# CAN included, on top of the existing falcon-yocto-build docker volume
# (see run.sh/NOTES.md for how that volume was first populated).
#
# Bind-mounts the shared, board-agnostic Qt app source
# (../ultima-app/src) read-only into the container —
# see meta-ultima-beagleplay-src/recipes-ultima/ultima-app/ultima-app.bb,
# which copies it into WORKDIR before building rather than building in
# place, so this mount can stay read-only and the shared source tree is
# never touched by the Yocto build. Same pattern for ../test/avm-benchmark
# (see recipes-avm/avm-benchmark/avm-benchmark.bb) — a standalone
# CMake/Qt5 bitbake target, not part of tisdk-base-image's IMAGE_INSTALL,
# built with `./build.sh avm-benchmark`. Same pattern again for
# ../../mycam004m (see recipes-kernel/mycam004m/mycam004m.bb) — a separate
# repo, sibling of this UltimaGC checkout, holding the mycam004m V4L2
# camera driver (real + fake/mock backends).
#
# Usage: ./build.sh [bitbake-target]   (defaults to tisdk-base-image)

set -euo pipefail
cd "$(dirname "$0")"

IMAGE=falcon-yocto:latest
VOLUME=falcon-yocto-build
ULTIMA_APP_SRC="$(cd ../ultima-app/src && pwd)"
AVM_BENCHMARK_SRC="$(cd ../test/avm-benchmark && pwd)"
MYCAM004M_SRC="$(cd ../../mycam004m && pwd)"
TARGET="${1:-tisdk-base-image}"

# Sync this directory's layer into the volume — the volume is the only
# place bitbake actually reads from (see run.sh for why it's not a host
# bind mount for the whole build tree).
docker run --rm \
  -v "$VOLUME:/home/builder/yocto" \
  -v "$(pwd)/meta-ultima-beagleplay-src:/incoming:ro" \
  -u root "$IMAGE" bash -c '
    set -e
    rm -rf /home/builder/yocto/tisdk/sources/meta-ultima-beagleplay
    cp -a /incoming /home/builder/yocto/tisdk/sources/meta-ultima-beagleplay
    chown -R builder:builder /home/builder/yocto/tisdk/sources/meta-ultima-beagleplay
    grep -q meta-ultima-beagleplay /home/builder/yocto/tisdk/build/conf/bblayers.conf || {
      echo "meta-ultima-beagleplay not in bblayers.conf — add it manually once (see NOTES.md)" >&2
      exit 1
    }
  '

# -t only when there is a terminal, so this stays runnable from a script or CI
# ("the input device is not a TTY" otherwise).
TTY_ARGS=()
[ -t 0 ] && TTY_ARGS=(-it) || true

# "${TTY_ARGS[@]+...}" rather than a bare "${TTY_ARGS[@]}": macOS ships bash
# 3.2, which throws "unbound variable" expanding a genuinely empty array
# under set -u (fixed upstream in bash 4.4) — this is the standard portable
# guard for that.
docker run --rm ${TTY_ARGS[@]+"${TTY_ARGS[@]}"} --cap-add SYS_PTRACE \
  -v "$VOLUME:/home/builder/yocto" \
  -v "$ULTIMA_APP_SRC:/home/builder/yocto/ultima-app-src:ro" \
  -v "$AVM_BENCHMARK_SRC:/home/builder/yocto/avm-benchmark-src:ro" \
  -v "$MYCAM004M_SRC:/home/builder/yocto/mycam004m-src:ro" \
  "$IMAGE" bash -c "
    source /home/builder/yocto/tisdk/sources/oe-core/oe-init-build-env /home/builder/yocto/tisdk/build
    bitbake $TARGET
  "

echo "Done. Pull images out with the docker cp command in NOTES.md's 'Build environment' section."
