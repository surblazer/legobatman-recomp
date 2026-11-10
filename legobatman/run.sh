#!/bin/sh
# Launch the Linux build of LEGO Batman (nfsmw-nx's ReXGlue SDK, Xenos GPU emulation over Vulkan).
# GPU emulation is on by default (src/legobatman_app.h). Extra arguments are passed through.
here="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$here/../tools/nfsmw-nx/sdk/out/linux-amd64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$here/userdata"
exec "$here/out/linux-nx/legobatman" \
    --game_data_root="$here/../game" \
    --user_data_root="$here/userdata" \
    "$@"
