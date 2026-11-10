#!/bin/bash
# Prepares the external trees this port builds against, at the commits it was made with, with our patches applied:
#   tools/nfsmw-nx  StevensND/nfsmw-nx 20f8dc1 (ReXGlue SDK 0.10 with the Horizon layer, GPL-3.0)
#                   + patches/nfsmw-nx-sdk-legobatman.patch, third-party code by its tools/fetch_thirdparty.py
#   tools/uam-src   devkitPro/uam 5a5afc2 (shader compiler) + patches/uam-legobatman.patch
#                   (SwitchWaker's scheduling patches and the zero-wins multiply option)
# Then: tools/switch-env/Containerfile (podman image), the NVK SDK (tools/mesa-switch, see nfsmw-nx docs/mesa.md),
# tools/switch-env/build-uam.sh, the codegen (legobatman/codegen.sh) and tools/switch-env/build-switch.sh.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root/tools"

fetch() {  # fetch <dir> <url> <commit> <patch>
    local dir=$1 url=$2 commit=$3 patch=$4
    if [ -d "$dir/.git" ]; then
        echo "$dir exists: left as it is (delete it to start over)"
        return 0
    fi
    git clone "$url" "$dir"
    git -C "$dir" checkout --quiet "$commit"
    git -C "$dir" apply "$root/patches/$patch"
    echo "$dir: $commit + $patch"
}

fetch nfsmw-nx https://github.com/StevensND/nfsmw-nx.git 20f8dc122bf0cd95211606ebacd3fda550cb21a8 nfsmw-nx-sdk-legobatman.patch
if [ ! -d nfsmw-nx/sdk/thirdparty/FFmpeg ]; then
    (cd nfsmw-nx && python3 tools/fetch_thirdparty.py)
fi
fetch uam-src https://github.com/devkitPro/uam.git 5a5afc2bae8b55409ab36ba45be63fcb73f68993 uam-legobatman.patch
echo "done. Next: the NVK SDK in tools/mesa-switch (nfsmw-nx docs/mesa.md), then README.md 'Build'."
