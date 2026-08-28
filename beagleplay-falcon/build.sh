#!/usr/bin/env bash
# Build tisdk-base-image for beagleplay-ti (default) or beagley-ai with
# ultima-app + Qt5 included, on top of the existing falcon-yocto-build
# docker volume (see run.sh/NOTES.md for how that volume was first
# populated).
#
# BOARD selects the machine and which sibling BitBake build directory gets
# used — tisdk/build (MACHINE=beagleplay-ti) or tisdk/build-beagley-ai
# (MACHINE=beagley-ai), matching NOTES.md's "Build setup used" section.
# Both meta-ultima-* layers are always synced into the volume regardless of
# BOARD — each only contributes override-scoped content the other
# machine's build never matches, so this is harmless for either board (see
# NOTES.md's BeagleY-AI bring-up section).
#
#   ./build.sh                        # beagleplay-ti, tisdk-base-image
#   BOARD=beagley-ai ./build.sh       # beagley-ai, tisdk-base-image
#   BOARD=beagley-ai ./build.sh avm-benchmark
#
# Bind-mounts the shared, board-agnostic Qt app source
# (../ultima-app) read-only into the container —
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
ULTIMA_APP_SRC="$(cd ../ultima-app && pwd)"
# Both optional: only avm-benchmark/mycam004m bitbake targets actually need
# their source, but the mount args below are unconditional either way, so a
# checkout without these sibling repos (e.g. one that only has UltimaApp,
# the Android companion app, alongside this one) shouldn't hard-fail a
# build of an unrelated target like ultima-app. Fall back to an empty dir
# under the volume itself so the bind-mount source always exists.
AVM_BENCHMARK_SRC="$(cd ../test/avm-benchmark 2>/dev/null && pwd || echo /tmp/ultima-empty-avm-benchmark)"
# ../../mycam004m assumes this checkout sits under ~/code/ alongside its
# sibling repos. When UltimaGC lives elsewhere (e.g. a mounted network
# share), that relative path resolves to nothing and silently falls back to
# the empty stub — mycam004m.bb's do_populate_lic then fails on a missing
# LICENSE, not a friendly "source not found" error. Try the relative path
# first, then the fixed ~/code/mycam004m location, before giving up.
# (Three separate assignments, not one chained `&&`/`||` one-liner — with a
# second `&& pwd` fallback appended after an `||`, `&&`/`||` precedence
# re-runs the first `pwd` a second time whenever the first `cd` already
# succeeded, duplicating the path. Each `cd ... || true` also matters on its
# own under `set -e`: without it, a failed `cd` inside the command
# substitution aborts the whole script right here instead of falling
# through to the next candidate.)
MYCAM004M_SRC="$(cd ../../mycam004m 2>/dev/null && pwd || true)"
MYCAM004M_SRC="${MYCAM004M_SRC:-$(cd ~/code/mycam004m 2>/dev/null && pwd || true)}"
MYCAM004M_SRC="${MYCAM004M_SRC:-/tmp/ultima-empty-mycam004m}"
mkdir -p /tmp/ultima-empty-avm-benchmark /tmp/ultima-empty-mycam004m
TARGET="${1:-tisdk-base-image}"

BOARD="${BOARD:-beagleplay-ti}"
case "$BOARD" in
  beagleplay-ti) BUILD_SUBDIR=build ;;
  beagley-ai)    BUILD_SUBDIR=build-beagley-ai ;;
  *)
    echo "Unknown BOARD '$BOARD' (expected beagleplay-ti or beagley-ai)" >&2
    exit 1
    ;;
esac
BEAGLEY_AI_SRC="$(cd ../beagley-ai/meta-ultima-beagley-ai-src && pwd)"

# Stage both layers to a local scratch dir before bind-mounting, excluding
# .smbdelete* — on an SMB-mounted checkout (e.g. a network share on
# macOS), a stray .smbdeleteAAA* rename-then-unlink artifact left behind by
# an earlier `rm`/`mv` can make a container's `cp -a` of the live path fail
# ("No such file or directory") mid-traversal, a race between directory
# enumeration and file access over the network share. These are already
# gitignored and harmless to skip. See NOTES.md's BeagleY-AI layer-split
# section for the same fix applied by hand during that resync.
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGE_DIR"' EXIT
rsync -a --exclude='.smbdelete*' meta-ultima-beagleplay-src/ "$STAGE_DIR/beagleplay/"
rsync -a --exclude='.smbdelete*' "$BEAGLEY_AI_SRC/" "$STAGE_DIR/beagley-ai/"

# Sync both boards' layers into the volume — the volume is the only place
# bitbake actually reads from (see run.sh for why it's not a host bind
# mount for the whole build tree). Also bootstraps tisdk/build-beagley-ai
# the first time (mechanical steps only — copy tisdk/build's conf/, flip
# MACHINE, comment out the beagleplay-only ti-falcon DISTROOVERRIDES line,
# point SSTATE_DIR at the shared cache. See NOTES.md "Build setup used".
# BBLAYERS itself is deliberately NOT auto-edited here, same as the
# meta-ultima-beagleplay check below always was — a fresh volume still
# needs that one-time manual line added, same reasoning either way: it's a
# deliberate, human-reviewed edit to a shared config, not mechanical setup.
docker run --rm \
  -v "$VOLUME:/home/builder/yocto" \
  -v "$STAGE_DIR/beagleplay:/incoming-beagleplay:ro" \
  -v "$STAGE_DIR/beagley-ai:/incoming-beagley-ai:ro" \
  -e "BOARD=$BOARD" \
  -u root "$IMAGE" bash -c '
    set -e
    YOCTO=/home/builder/yocto

    rm -rf "$YOCTO/tisdk/sources/meta-ultima-beagleplay"
    cp -a /incoming-beagleplay "$YOCTO/tisdk/sources/meta-ultima-beagleplay"
    chown -R builder:builder "$YOCTO/tisdk/sources/meta-ultima-beagleplay"

    rm -rf "$YOCTO/tisdk/sources/meta-ultima-beagley-ai"
    cp -a /incoming-beagley-ai "$YOCTO/tisdk/sources/meta-ultima-beagley-ai"
    chown -R builder:builder "$YOCTO/tisdk/sources/meta-ultima-beagley-ai"

    grep -q meta-ultima-beagleplay "$YOCTO/tisdk/build/conf/bblayers.conf" || {
      echo "meta-ultima-beagleplay not in tisdk/build/conf/bblayers.conf — add it manually once (see NOTES.md)" >&2
      exit 1
    }

    BBA="$YOCTO/tisdk/build-beagley-ai"
    if [ ! -f "$BBA/conf/local.conf" ]; then
      mkdir -p "$BBA/conf"
      cp "$YOCTO/tisdk/build/conf/local.conf" "$BBA/conf/local.conf"
      cp "$YOCTO/tisdk/build/conf/bblayers.conf" "$BBA/conf/bblayers.conf"
      sed -i \
        -e "s|^MACHINE ?= \"beagleplay-ti\"|MACHINE ?= \"beagley-ai\"|" \
        -e "s|^SSTATE_DIR = \"\${TOPDIR}/sstate-cache\"|SSTATE_DIR = \"$YOCTO/tisdk/build/sstate-cache\"|" \
        -e "s|^DISTROOVERRIDES:append = \":ti-falcon\"|# DISTROOVERRIDES:append = \":ti-falcon\"  # removed for beagley-ai: Falcon not wired for j722s (bb.org u-boot, not TI-staging)|" \
        "$BBA/conf/local.conf"
      chown -R builder:builder "$BBA"
      echo "Bootstrapped $BBA from tisdk/build (see NOTES.md \"Build setup used\")." >&2
    fi

    if [ "$BOARD" = "beagley-ai" ]; then
      grep -q meta-ultima-beagley-ai "$BBA/conf/bblayers.conf" || {
        echo "meta-ultima-beagley-ai not in tisdk/build-beagley-ai/conf/bblayers.conf — add it manually once, alongside the existing meta-ultima-beagleplay entry (see NOTES.md)" >&2
        exit 1
      }
    fi
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
    source /home/builder/yocto/tisdk/sources/oe-core/oe-init-build-env /home/builder/yocto/tisdk/$BUILD_SUBDIR
    bitbake $TARGET
  "

echo "Done ($BOARD, tisdk/$BUILD_SUBDIR). Pull images out with the docker cp command in NOTES.md's 'Build environment' section (deploy dir is tisdk/$BUILD_SUBDIR/deploy-ti/images/<machine>/)."
