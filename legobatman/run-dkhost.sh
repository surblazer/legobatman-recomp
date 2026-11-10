#!/bin/sh
# Debug host: the Switch deko3d renderer on Linux over OpenGL (out/linux-nx configured with
# -DLEGOBATMAN_DK_GL_HOST=ON). Shaders: translated GLSL from out/shaders/glsl plus the present pair, gathered in
# out/host-shaders/ (refresh after tools/switch-env/stage-shaders.sh). Frames: set CVHD_DK_GL_DUMP=<dir>:<every N>.
# After rebuilding, copy the SDK's fresh librexgpu-xenos.so next to the executable (the build only refreshes that
# copy when the game relinks).
here="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$here/out/host-shaders"
cp "$here"/out/shaders/glsl/*.glsl "$here/out/host-shaders/" 2>/dev/null
cp "$here/src/gpu/shaders/present.vert" "$here/out/host-shaders/present.vert.glsl"
cp "$here/src/gpu/shaders/present.frag" "$here/out/host-shaders/present.frag.glsl"
cp "$here/../tools/nfsmw-nx/sdk/out/linux-amd64/librexgpu-xenos.so" "$here/out/linux-nx/"
exec "$here/run.sh" --gpu_backend=deko3d --deko3d_shader_dir="$here/out/host-shaders/" "$@"
