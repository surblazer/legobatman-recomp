#!/bin/sh
# Builds the offline shader extractor (host tools, Linux).
#   tt_shader_extract <outdir> GAME.DAT GAME1.DAT ...   every shader in the game's archives (LZ2K-decoded), named by
#                                                       the FNV-1a hash the renderer looks up
#   disasm_dump <indir> <outdir>                        the SDK's disassembly of each (the input stage-shaders.sh reads)
# Pixel shaders go to work/shader_dumps_extracted/. Vertex shaders are not usable as extracted: D3D rewrites their
# vertex fetches at bind time, so the hashes differ from what the game runs (see PORTING_NOTES.md).
set -e
cd "$(dirname "$0")"
SDK=../nfsmw-nx/sdk
cc -O2 -o tt_shader_extract tt_shader_extract.c
cc -O2 -o ucode_scan ucode_scan.c
# Shader is not exported from the SDK's shared libraries: its sources are compiled in, and librexruntime supplies
# the rest. Needs the host SDK build (preset linux-amd64).
INC="-I$SDK/include -I$SDK/out/linux-amd64/generated"
for d in $SDK/thirdparty/*/include; do INC="$INC -I$d"; done
P=$SDK/src/graphics/pipeline/shader
clang++ -std=c++23 -O2 -DNDEBUG -DREX_PLATFORM_LINUX=1 $INC -o disasm_dump disasm_dump.cpp \
  $P/shader.cpp $P/translator.cpp $P/translator_disasm.cpp $SDK/src/graphics/format/ucode.cpp \
  -L$SDK/out/linux-amd64 -lrexruntime -Wl,-rpath,"$(cd $SDK/out/linux-amd64 && pwd)"
