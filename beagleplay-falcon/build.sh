#!/usr/bin/env bash
# Build tisdk-base-image for beagleplay-ti with ultima-app + Qt5 (linuxfb) +
# CAN included, on top of the existing falcon-yocto-build docker volume
# (see run.sh/NOTES.md for how that volume was first populated).
#
# Bind-mounts the shared, board-agnostic Qt app source
# (br2-external/package/ultima-app/src) read-only into the container —
# see meta-ultima-beagleplay-src/recipes-ultima/ultima-app/ultima-app.bb,
# which copies it into WORKDIR before building rather than building in
# place, so this mount can stay read-only and the shared source tree is
# never touched by the Yocto build.
#
# Usage: ./build.sh [bitbake-target]   (defaults to tisdk-base-image)

set -euo pipefail
cd "$(dirname "$0")"

IMAGE=falcon-yocto:latest
VOLUME=falcon-yocto-build
ULTIMA_APP_SRC="$(cd ../br2-external/package/ultima-app/src && pwd)"
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

docker run --rm -it --cap-add SYS_PTRACE \
  -v "$VOLUME:/home/builder/yocto" \
  -v "$ULTIMA_APP_SRC:/home/builder/yocto/ultima-app-src:ro" \
  "$IMAGE" bash -c "
    source /home/builder/yocto/tisdk/sources/oe-core/oe-init-build-env /home/builder/yocto/tisdk/build
    bitbake $TARGET
  "

echo "Done. Pull images out with the docker cp command in NOTES.md's 'Build environment' section."
