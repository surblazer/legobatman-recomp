#!/bin/bash
# Sends legobatman.nro to the console's hbmenu netloader (press Y in hbmenu) and starts it. The NRO is written to
# sdmc:/switch/legobatman/legobatman.nro, next to game_root/ and shaders/, so it starts as usual. hbmenu writes under
# sdmc:/switch/ and strips leading slashes, so the path given is "legobatman/legobatman.nro".
# Console address: env LB_HOST or tools/switch-env/switch_host.txt.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
host="${LB_HOST:-$(head -1 "$root/tools/switch-env/switch_host.txt")}"
exec podman run --rm --network host -v "$root":/work:Z -w /work localhost/legobatman-switch-env \
  /opt/devkitpro/tools/bin/nxlink -a "$host" -p legobatman/legobatman.nro legobatman/out/switch/legobatman.nro
