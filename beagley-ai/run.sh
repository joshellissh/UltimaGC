#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

IMAGE=falcon-yocto:latest
VOLUME=falcon-yocto-build   # Docker-managed volume (ext4 in the VM) — NOT a host
                             # bind mount. OE-core's sanity checker refuses to run
                             # on APFS because it's case-insensitive, and virtiofs
                             # bind mounts are far slower for Yocto's I/O pattern.

docker build -t "$IMAGE" .
docker volume create "$VOLUME" >/dev/null

docker run -it --rm \
  --cap-add SYS_PTRACE \
  -v "$VOLUME:/home/builder/yocto" \
  "$IMAGE"
# SYS_PTRACE: pseudo (bitbake's fakeroot) falls back to ptrace-based
# syscall interception for some operations; without this capability
# do_package can fail with "got *at() syscall for unknown directory".

# To pull the built images out to the host for flashing:
#   docker run --rm -v falcon-yocto-build:/src -v "$(pwd)/deploy-beagley-ai:/dst" \
#     falcon-yocto:latest bash -c \
#     'cp -a /src/tisdk/build-beagley-ai/arago-tmp-default-glibc/deploy/images/beagley-ai/. /dst/'
