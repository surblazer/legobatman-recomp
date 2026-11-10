#!/bin/bash
# Translate the game's shader dumps to DKSH for the deko3d backend, straight into the NRO's romfs (the SD card needs no
# shaders folder; the backend reads romfs:/shaders/ first):
#   legobatman/out/romfs/shaders/{present.vert,present.frag,shader_<hash>.<stage>}.dksh
#
# Inputs (any number of dump folders, merged):
#   work/shader_dumps            SDK --dump_shaders output (named by the SDK's own hash)
#   work/shader_dumps_missing    copies of sdmc:/switch/legobatman/shaders/missing/ from console runs
#   work/shader_dumps_extracted  every pixel shader on the disc (tools/shader-extract: LZ2K + microcode scan)
#                                (already named by the backend's hash)
#   work/shader_dumps_extracted_vs  every vertex shader on the disc, translated as generic shaders (step 4)
# The backend looks a shader up by FNV-1a of the guest ucode bytes (big endian, src/gpu/ucode_hash.h); the SDK's
# .ucode.bin dump is the byte-swapped copy, so names are recomputed from it. Translator: tools/cvhd-recomp's
# xenos2glsl.py (GLSL) + devkitPro's uam (DKSH), both run inside the legobatman-switch-env container.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
exec podman run --rm -v "$root":/work:Z -w /work localhost/legobatman-switch-env bash -c '
set -euo pipefail
# The patched uam (tools/uam-src: devkitPro uam 5a5afc2 + SwitchWaker scheduling patches 6-8) when built: the stock
# one emits Maxwell scheduling that made shaders wrong on the console only (flicker). Build it with
# tools/switch-env/build-uam.sh.
if [ -x tools/uam-src/build/uam ]; then export PATH=/work/tools/uam-src/build:$PATH; fi
echo "uam: $(command -v uam)"
# Hardware zero-wins multiply (FMZ): plain GLSL * and dot() from the translator, D3D9 semantics from uam.
# export UAM_MUL_ZERO_WINS=1 XENOS_NATIVE_ZERO_WINS=1   # OFF: brought the flicker back on the console (2026-11-10); hardware FMZ differs from Xenos somewhere
# FINITE_SPECIAL: rcp/rsq/log/exp return +-2^60 instead of Inf and multiplies are plain wherever the taint analysis
# finds no Inf source; the backend clamps the float constants of the game to +-2^60 as they are written (it uploads
# ~20,000 huge/Inf values a second), which made this set flicker-free on the console (2026-11-10). Busy shaders about
# half the size of the all-exact set (alternative set 1 below).
# Default: every multiply exact. The fast set (FINITE_SPECIAL: rcp/rsq/log/exp, texels, vertex data, outputs and
# constants clamped to +-2^60, plain multiplies where no Inf source remains) kept flickering on the console and gained
# little fps (2026-11-10): dropped as the default, kept as alternative set 1 (with debug flag 15 for the constants).
export XENOS_EXACT_MULS=1
named=legobatman/out/shaders/named
glsl=legobatman/out/shaders/glsl
stage=legobatman/out/romfs/shaders
rm -rf "$named" "$glsl" "$stage"
mkdir -p "$named" "$glsl" "$stage"

# 1. name every dump by the backend hash
python3 -I - work/shader_dumps work/shader_dumps_missing work/shader_dumps_extracted "$named" <<"EOF"
import glob, os, shutil, struct, sys
*srcs, out = sys.argv[1:]
n = 0
for d in srcs:
    for b in glob.glob(os.path.join(d, "*.ucode.bin.*")):
        ext = b.rsplit(".", 1)[1]
        text = b.replace(".ucode.bin.", ".ucode.")
        if not os.path.exists(text):
            continue
        data = open(b, "rb").read()
        words = struct.unpack("<%dI" % (len(data) // 4), data)
        raw = struct.pack(">%dI" % len(words), *words)  # back to guest (big-endian) byte order
        h = 1469598103934665603
        for c in raw:
            h ^= c
            h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        stem = "shader_%016X" % h
        shutil.copy(text, os.path.join(out, "%s.ucode.%s" % (stem, ext)))
        shutil.copy(b, os.path.join(out, "%s.ucode.bin.%s" % (stem, ext)))
        n += 1
print("named", n, "dumps")
EOF

# 2. translate + compile, 3. stage
python3 -I tools/scripts/xenos2glsl.py --all "$named" --out-dir "$glsl" --check > "$glsl/../translate.log" 2>&1 || true
grep -E "^FAIL" "$glsl/../translate.log" | sed "s/:.*//" | sort | uniq -c | sort -rn | head -5 || true
tail -1 "$glsl/../translate.log"
for s in vert frag; do
    uam -s $s -o "$stage/present.$s.dksh" legobatman/src/gpu/shaders/present.$s
done
for f in "$glsl"/*.glsl.dksh; do
    [ -e "$f" ] || continue
    cp "$f" "$stage/$(basename "$f" .glsl.dksh).dksh"
done
echo "romfs: $(ls "$stage" | wc -l) DKSH files in $stage (packed into the NRO by CMakeLists.txt)"
# 4. Generic vertex shaders: the disc copies (work/shader_dumps_extracted_vs, from tools/shader-extract), named by the
# fetch-agnostic key (tools/scripts/vs_generic_key.py) and translated with XENOS_GENERIC_FETCH=1. The backend falls
# back to shader_<key>.gvert/.ggeom.dksh when it has no exact DKSH for a bound vertex shader; shader_<key>.gmap holds
# (register, write mask) per vertex input, in input order.
gnamed=legobatman/out/shaders/named_generic
gglsl=legobatman/out/shaders/glsl_generic
rm -rf "$gnamed" "$gglsl" && mkdir -p "$gnamed" "$gglsl"
if [ -d work/shader_dumps_extracted_vs ]; then
python3 -I - work/shader_dumps_extracted_vs "$gnamed" <<"EOF"
import glob, os, shutil, sys
sys.path.insert(0, "tools/scripts")
from vs_generic_key import key_of_bin
src, out = sys.argv[1:]
keys = set()
for b in sorted(glob.glob(os.path.join(src, "*.ucode.bin.vert"))):
    text = b.replace(".ucode.bin.", ".ucode.")
    if not os.path.exists(text):
        continue
    key, fetches = key_of_bin(b)
    if key in keys or not fetches:
        continue
    keys.add(key)
    shutil.copy(text, os.path.join(out, "shader_%016X.ucode.vert" % key))
print("generic vertex shaders:", len(keys), "keys")
EOF
XENOS_GENERIC_FETCH=1 python3 -I tools/scripts/xenos2glsl.py --all "$gnamed" --out-dir "$gglsl" --check > "$gglsl/../translate_generic.log" 2>&1 || true
tail -1 "$gglsl/../translate_generic.log"
python3 -I - "$gglsl" "$stage" <<"EOF"
import glob, json, os, shutil, sys
src, stage = sys.argv[1:]
n = 0
for j in glob.glob(os.path.join(src, "*.vert.json")):
    stem = os.path.basename(j)[:-len(".vert.json")]
    vert = os.path.join(src, stem + ".vert.glsl.dksh")
    geom = os.path.join(src, stem + ".geom.glsl.dksh")
    if not os.path.exists(vert):
        continue
    shutil.copy(vert, os.path.join(stage, stem + ".gvert.dksh"))
    if os.path.exists(geom):
        shutil.copy(geom, os.path.join(stage, stem + ".ggeom.dksh"))
    attrs = json.load(open(j))["attributes"]
    open(os.path.join(stage, stem + ".gmap"), "wb").write(bytes(v for a in attrs for v in (a["reg"], a["mask"])))
    n += 1
print("romfs: %d generic vertex shaders" % n)
EOF
fi
# Alternative sets for debug server "flag 3 N" (romfs:/shaders_altN/), A/B on the console:
#   1: FINITE_SPECIAL for vertex shaders only (pixel shaders exact: the backend falls back to the main set for a
#      shader the alternative directory lacks). The full fast set flickered; this measures the vertex shading part.
build_alt() {
    local n=$1; shift
    local alt=legobatman/out/romfs/shaders_alt$n out=legobatman/out/shaders/glsl_alt$n
    rm -rf "$alt" "$out" && mkdir -p "$alt" "$out"
    env "$@" python3 -I tools/scripts/xenos2glsl.py --all "$named" --out-dir "$out" > /dev/null 2>&1 || true
    for f in "$out"/*.glsl; do
        case $f in *.vert.glsl) s=vert;; *.frag.glsl) s=frag;; *) continue;; esac
        uam -s $s -o "$alt/$(basename "$f" .glsl).dksh" "$f" || true
    done
    echo "romfs alt$n: $(ls "$alt" | wc -l) DKSH files"
}
rm -rf legobatman/out/romfs/shaders_alt
rm -rf legobatman/out/romfs/shaders_alt2
build_alt 1 XENOS_FINITE_SPECIAL=1 XENOS_EXACT_MULS=0
rm -f legobatman/out/romfs/shaders_alt1/*.frag.dksh
echo "romfs alt1 (vertex shaders only): $(ls legobatman/out/romfs/shaders_alt1 | wc -l) DKSH files"
'
