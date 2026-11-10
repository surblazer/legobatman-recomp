#!/bin/bash
# Builds legobatman.nro inside the legobatman-switch-env container.
# Usage (from the project root): tools/switch-env/build-switch.sh [extra cmake --build args]
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm -v "$root":/work:Z -w /work localhost/legobatman-switch-env bash -c '
set -euo pipefail
if [ ! -f legobatman/out/switch/build.ninja ]; then
  cmake -S legobatman -B legobatman/out/switch -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=/work/tools/nfsmw-nx/tools/switch/cmake/switch-devkitA64.cmake \
    -DREXSDK_DIR=/work/tools/nfsmw-nx/sdk \
    -DREXGLUE_SWITCH_NVK_SDK=/work/tools/mesa-switch/mesa-linux-install/opt/devkitpro/portlibs/switch
fi
cmake --build legobatman/out/switch -j"$(nproc)" "$@"
' bash "$@"
