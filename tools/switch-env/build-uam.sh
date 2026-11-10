#!/bin/bash
# Builds the patched uam (tools/uam-src: devkitPro/uam 5a5afc2 plus SwitchWaker's scheduling patches 6, 7, 8, marked
# "LEGO Batman port" in mesa-imported/codegen) inside the legobatman-switch-env container -> tools/uam-src/build/uam.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm -v "$root":/work:Z -w /work/tools/uam-src localhost/legobatman-switch-env bash -c '
MP=$(python3 -c "import mako,os;print(os.path.dirname(os.path.dirname(mako.__file__)))")
export PYTHONPATH=$MP   # meson runs from /opt/pymeson, which has no mako
[ -f build/build.ninja ] || meson setup build --buildtype=release
ninja -C build -j"$(nproc)"
'
