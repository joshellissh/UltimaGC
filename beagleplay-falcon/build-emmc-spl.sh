#!/usr/bin/env bash
# Build the eMMC-targeting variant of the falcon R5 SPL.
#
# Falcon mode picks its boot device at *compile* time, not from wherever the
# ROM happened to load the SPL: arch/arm/mach-k3/r5/common.c's
# k3_r5_falcon_bootmode() reads the `mmcdev` environment variable, and
# arch/arm/mach-k3/common.c's k3_falcon_fdt_fixup() separately reads `boot` +
# `bootpart` to derive the kernel's root=PARTUUID. There is no writable
# environment in this build (every CONFIG_ENV_IS_IN_* is unset), so both are
# baked in from board/beagle/beagleplay/beagleplay.env. That means SD and eMMC
# need two *different* tiboot3.bin builds:
#
#   | beagleplay.env | SD (default build) | eMMC |
#   |----------------|--------------------|------|
#   | mmcdev         | 1                  | 0    |
#   | bootpart       | 1:2                | 0:2  |
#
# This script temporarily flips those two values in the u-boot source inside
# the build volume, recompiles just the R5 SPL, extracts the result, then puts
# the source back and recompiles again so the volume is left producing the SD
# variant. The normal build.sh path is untouched and keeps building SD images.
#
# Output: deploy-falcon/tiboot3-falcon-emmc.bin  (goes in the eMMC's boot0
# hardware partition — see emmc-install.sh)
#
# Requires build.sh to have been run at least once (the u-boot workdir has to
# exist in the volume).

set -euo pipefail
cd "$(dirname "$0")"

IMAGE=falcon-yocto:latest
VOLUME=falcon-yocto-build
OUT="deploy-falcon/tiboot3-falcon-emmc.bin"

mkdir -p deploy-falcon

docker run --rm --cap-add SYS_PTRACE \
  -v "$VOLUME:/home/builder/yocto" \
  -v "$(pwd)/deploy-falcon:/out" \
  "$IMAGE" bash -euo pipefail -c '
    Y=/home/builder/yocto

    # The R5 (k3r5 multiconfig) u-boot workdir. Globbed rather than hardcoded
    # so a u-boot version bump does not silently break this.
    W=$(echo $Y/tisdk/build/arago-tmp-default-baremetal-k3r5/work/beagleplay_ti_k3r5-oe-eabi/u-boot-ti-staging/*/)
    if [ ! -d "$W/git/board/beagle/beagleplay" ]; then
      echo "No R5 u-boot workdir in the build volume — run ./build.sh first." >&2
      exit 1
    fi
    ENV_FILE="$W/git/board/beagle/beagleplay/beagleplay.env"

    # Never leave the source tree flipped to the eMMC values, whatever happens —
    # otherwise the next build.sh silently bakes an eMMC-targeting SPL into an
    # SD image.
    restore_env() { sed -i "s/^mmcdev=0$/mmcdev=1/; s/^bootpart=0:2$/bootpart=1:2/" "$ENV_FILE"; }
    trap restore_env EXIT

    compile() {
      # The falcon/R5 SPL lives entirely under the k3r5 BitBake multiconfig
      # (meta-ti-bsp/conf/multiconfig/k3r5.conf, auto-enabled via
      # BBMULTICONFIG in mc_k3r5.inc). Unscoped u-boot-ti-staging builds the
      # unrelated A53 U-Boot instead.
      # set +u around the source: oe-init-build-env reads $BBSERVER unguarded.
      set +u
      source $Y/tisdk/sources/oe-core/oe-init-build-env $Y/tisdk/build >/dev/null
      set -u
      bitbake -c compile -f mc:k3r5:u-boot-ti-staging
    }

    check() {  # check <expected-mmcdev> <expected-bootpart> <file>
      got=$(strings "$3" | grep -E "^mmcdev=|^bootpart=" | sort | tr "\n" " ")
      want="bootpart=$2 mmcdev=$1 "
      [ "$got" = "$want" ] || { echo "SPL env mismatch in $3: got [$got] want [$want]" >&2; exit 1; }
    }

    echo "=== Building eMMC variant (mmcdev=0, bootpart=0:2) ==="
    sed -i "s/^mmcdev=1$/mmcdev=0/; s/^bootpart=1:2$/bootpart=0:2/" "$ENV_FILE"
    grep -E "^mmcdev=|^bootpart=" "$ENV_FILE"
    compile
    check 0 0:2 "$W/build/tiboot3.bin"
    cp -L "$W/build/tiboot3.bin" /out/tiboot3-falcon-emmc.bin
    chmod 644 /out/tiboot3-falcon-emmc.bin

    echo "=== Restoring SD variant (mmcdev=1, bootpart=1:2) ==="
    restore_env
    grep -E "^mmcdev=|^bootpart=" "$ENV_FILE"
    compile

    # Hard invariant: the volume must be left building the SD variant. If this
    # is wrong, the next build.sh bakes an eMMC-targeting SPL into the SD .wic,
    # which fails later in a thoroughly confusing way ("MMC: no card present"
    # from an SD card that is plainly present).
    check 1 1:2 "$W/build/tiboot3.bin"
    check 1 1:2 $Y/tisdk/build/deploy-ti/images/beagleplay-ti/tiboot3.bin

    # -c compile -f leaves a taint stamp that forces u-boot to rebuild on every
    # subsequent bitbake run. The source is back to its original state, so drop
    # it; the next build recomputes the untainted signature normally.
    rm -f $Y/tisdk/build/arago-tmp-default-baremetal-k3r5/stamps/beagleplay_ti_k3r5-oe-eabi/u-boot-ti-staging/*.do_compile.taint
  '

echo
echo "Wrote $OUT"
strings "$OUT" | grep -E '^mmcdev=|^bootpart='
