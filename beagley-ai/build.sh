#!/usr/bin/env bash
# Build tisdk-base-image for BeagleY-AI (AM67A/J722S) with ultima-app + Qt5,
# on top of the existing falcon-yocto-build docker volume (see run.sh and the
# BeagleY-AI notes for how that volume was first populated).
#
# The docker image and volume are still named "falcon-yocto*" — a legacy name
# from an earlier build of this project. Kept as-is deliberately: renaming
# them would orphan the populated sstate cache and the whole build tree.
# BeagleY-AI boots via Falcon too, so the name still fits.
#
#   ./build.sh                     # tisdk-base-image (default)
#   ./build.sh <bitbake-target>
#
# Bind-mounts the shared, board-agnostic Qt app source (../ultima-app)
# read-only into the container — see
# meta-ultima-beagley-ai-src/recipes-ultima/ultima-app/ultima-app.bb, which
# copies it into WORKDIR before building rather than building in place, so the
# mount can stay read-only and the shared source tree is never touched by the
# Yocto build.

set -euo pipefail
cd "$(dirname "$0")"

IMAGE=falcon-yocto:latest
VOLUME=falcon-yocto-build
BUILD_SUBDIR=build-beagley-ai
ULTIMA_APP_SRC="$(cd ../ultima-app && pwd)"
CAMDRIVER_SRC="$(cd ../camdriver && pwd)"
BEAGLEY_AI_SRC="$(cd meta-ultima-beagley-ai-src && pwd)"
TARGET="${1:-tisdk-base-image}"

# Stage the layer to a local scratch dir before bind-mounting, excluding
# .smbdelete* — on an SMB-mounted checkout (e.g. a network share on macOS), a
# stray .smbdeleteAAA* rename-then-unlink artifact left behind by an earlier
# rm/mv can make the container's `cp -a` of the live path fail ("No such file
# or directory") mid-traversal, a race between directory enumeration and file
# access over the network share. These are gitignored and harmless to skip.
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGE_DIR"' EXIT
rsync -a --exclude='.smbdelete*' "$BEAGLEY_AI_SRC/" "$STAGE_DIR/beagley-ai/"

# Sync the layer into the volume — the volume is the only place bitbake
# actually reads from (see run.sh for why it's not a host bind mount for the
# whole build tree), and check that the build dir's BBLAYERS is set up.
# BBLAYERS itself is deliberately NOT auto-edited here — it's a one-time,
# human-reviewed manual edit to a shared config (see the BeagleY-AI notes).
docker run --rm \
  -v "$VOLUME:/home/builder/yocto" \
  -v "$STAGE_DIR/beagley-ai:/incoming-beagley-ai:ro" \
  -u root "$IMAGE" bash -c '
    set -e
    YOCTO=/home/builder/yocto

    rm -rf "$YOCTO/tisdk/sources/meta-ultima-beagley-ai"
    cp -a /incoming-beagley-ai "$YOCTO/tisdk/sources/meta-ultima-beagley-ai"
    chown -R builder:builder "$YOCTO/tisdk/sources/meta-ultima-beagley-ai"

    BBA="$YOCTO/tisdk/build-beagley-ai"
    if [ ! -f "$BBA/conf/bblayers.conf" ]; then
      echo "$BBA/conf/bblayers.conf missing — set the build dir up once (see the BeagleY-AI notes)." >&2
      exit 1
    fi
    grep -q meta-ultima-beagley-ai "$BBA/conf/bblayers.conf" || {
      echo "meta-ultima-beagley-ai not in $BBA/conf/bblayers.conf — add it manually once (see the BeagleY-AI notes)." >&2
      exit 1
    }
  '

# -t only when there is a terminal, so this stays runnable from a script or CI
# ("the input device is not a TTY" otherwise).
TTY_ARGS=()
[ -t 0 ] && TTY_ARGS=(-it) || true

# "${TTY_ARGS[@]+...}" rather than a bare "${TTY_ARGS[@]}": macOS ships bash
# 3.2, which throws "unbound variable" expanding a genuinely empty array under
# set -u (fixed upstream in bash 4.4) — the standard portable guard for that.
docker run --rm ${TTY_ARGS[@]+"${TTY_ARGS[@]}"} --cap-add SYS_PTRACE \
  -v "$VOLUME:/home/builder/yocto" \
  -v "$ULTIMA_APP_SRC:/home/builder/yocto/ultima-app-src:ro" \
  -v "$CAMDRIVER_SRC:/home/builder/yocto/camdriver-src:ro" \
  "$IMAGE" bash -c "
    source /home/builder/yocto/tisdk/sources/oe-core/oe-init-build-env /home/builder/yocto/tisdk/$BUILD_SUBDIR
    bitbake $TARGET
  "

echo "Done (beagley-ai, tisdk/$BUILD_SUBDIR). Pull images out with the docker cp command in the BeagleY-AI notes (deploy dir is tisdk/$BUILD_SUBDIR/deploy-ti/images/beagley-ai/)."
