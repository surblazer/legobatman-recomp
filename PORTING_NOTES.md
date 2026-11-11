# LEGO Batman (Xbox 360) on Switch: porting notes

How the port works, what was measured, and what was tried and dropped. Dates are when a finding was made (2026-11).
The README covers installing and building.

## Known issues

- **Flickering may still happen in places not yet played.** The flicker found so far came from Xenos arithmetic
  rules (0 × Inf = 0 and friends) that the shader translator now follows, but the translation is not proven exact
  for every shader, and only the intro, the first levels and the menus were checked.
- **Missing shaders are still possible.** Every shader on the disc is translated ahead of time, except: 15 pixel
  shaders the extractor could not tell apart from data, 46 vertex shaders that write to memory (memexport, not
  supported), one vertex shader with an unknown control-flow form, and shaders that are not stored in the
  archives the extractor reads (at least one pixel shader, 144ADA1C0F3B7DE7, came only from play). A shader the
  port lacks draws nothing, and the game saves its code to `shaders/missing/` next to the NRO.

## The executable

- XEX2, retail-encrypted, LZX-compressed. Original name `legogame_Final_360.exe`, base 0x82000000, entry 0x8238A438,
  image size 0xA80000.
- Code: `.text` 820E0000-826ECA8C plus 10 `.embsec_*` code sections (to 827464C4), about 1.68 M instructions.
- 156 imports, from `xboxkrnl.exe` and `xam.xex` only.
- Save/restore helpers: savegprlr_14 8237B560, restgprlr_14 8237B5B0, savefpr_14 8237BB40, restfpr_14 8237BB8C,
  savevmx_14 826BBA40, restvmx_14 826BBCD8, savevmx_64 826BBAD4, restvmx_64 826BBD6C.
- setjmp 826BB770, longjmp 826BB330 (replaced by the SDK's native ones: `setjmp_address` / `longjmp_address`).

## Recompilation

The port runs on the ReXGlue SDK through nfsmw-nx's fork (`tools/nfsmw-nx/sdk`: ReXGlue 0.10.0 plus a Horizon
layer). The game's code is recompiled to C++ by the SDK's `rexglue codegen`. Always regenerate with
`legobatman/codegen.sh`: a bare codegen silently undoes the steps after it.

- **Function boundaries.** The engine is built without RTTI, so the SDK's vtable scanner finds nothing, and virtual
  methods and callbacks with no .pdata entry were merged into their neighbours ("call to invalid or unregistered
  function"). `tools/scripts/find_code_pointers.py` finds code addresses stored in .rdata/.data or built with
  lis+addi/ori and writes about 3,200 sized entries to `functions_data_refs.toml` (generated: regenerate with
  `codegen.sh --scan`, don't hand-edit). Its inputs in `work/` (decrypted image, .pdata, imports, jump tables) came
  from XenonRecomp's XenonAnalyse and a small XenonUtils-based dump tool; they are not in the repository.
  `overrides.toml` holds the hand-written entries; it is included last, so its entries win.
- **Register locals** (`cr`/`xer`/`ctr`/`non_volatile_as_local` in overrides.toml). Hand-written vector routines
  without a prologue break under them in two ways: they read registers their caller set up (7 routines, inputs in
  v64+), or they write non-volatile registers as outputs (28 routines; this caused artefacts in the first story
  cutscene, found by bisecting one option at a time). Both kinds are marked `share_registers`. codegen.sh runs
  `check_reads_before_writes.py` and `check_nonvolatile_writers.py` and stops on anything new.
  `share_registers` on a normal function is wrong: its register values leak back into the caller (seen as a null
  write in sub_82546F48).
- **Direct calls:** `tools/scripts/direct_calls.py` rewrites about 72,400 `sub_X(ctx, base)` calls to
  `__imp__sub_X`. Hooked functions must keep their weak-alias calls (`--keep`).
- **`skip_msr = true`:** the game's 118 `mtmsrd` sites each took the SDK's global lock.
- **Floating point:** the game code and the app build with `-ffp-contract=off -fno-strict-aliasing` (clang:
  `-ffp-model=strict`). The SDK sets these only inside its own directory. Without them, GCC/clang fuse multiply-adds
  on aarch64 and the results drift from the PowerPC's.
- **Switch build:** LTO (`LEGOBATMAN_LTO`, `-flto=10`); an incremental build with the LTO link takes about 90 s.

## The Switch runtime (SDK changes, `patches/nfsmw-nx-sdk-legobatman.patch`)

- **Memory:** the game writes into the physical views (0xE0000000) at addresses the Switch memory layer had not
  committed (a memcpy in the first story cutscene). The fault handler now commits the surrounding 2 MB block on demand.
- **Locks:** the global critical region is a recursive mutex whose contended handoff goes through the kernel. The
  main thread spent ~36% blocked in it. Fixed with adaptive locking (spin on try_lock with `yield` before blocking),
  plus skip_msr above.
- **Busy-waits:** the game's D3D ring wait (sub_82396360) is hooked with a 100 µs sleep
  (`src/legobatman_waits.cpp`), and the command processor's WAIT_REG_MEM poll sleeps 100 µs (the value cvhd-recomp
  measured as best). Together with skip_msr: the first level loads in 9 s instead of 78 s.
- **Kernel stubs:** KeSaveFloatingPointState/KeRestoreFloatingPointState are silent no-ops. The game's GPU interrupt
  handler calls them on every interrupt, and logging them blocked the GPU thread on the log queue.
- **Logging:** async on the Switch. Log files are off unless `logging = true` in legobatman.toml (development
  builds default to on). The SDK asks the game through a weak `RexSwitchLogFiles()` before it redirects stderr or
  writes its first performance report; the game reads that key itself, since the config loader runs later. Crash
  and assert logs are written either way.
- **SaltyNX:** the swap reports frames and the 3D resolution to Status Monitor (`RexSwitchPerfCount`,
  `RexSwitchPerfResolution`).

## Renderer

The SDK's Xenos emulation over Vulkan (NVK) ran at about 2 fps on the console: EDRAM emulation costs 31-929 transfer
calls per frame, the same wall cvhd-recomp hit. The port uses cvhd-recomp's native deko3d backend instead
(`legobatman/src/gpu/`, from cvhd-recomp c15926f; `UPSTREAM.txt`). It sits behind the SDK's own command processor
(ring, PM4, register file, shader analysis, texture layouts) and replaces only the drawing: render targets are deko3d
images keyed by EDRAM base and pitch, resolves are image copies, and textures at a resolve destination alias the
image. Changes made for this game are marked "LEGO Batman port" in the source.

- **Depth and stencil:** deko3d numbers compare functions from 1, Xenos from 0. Passing them unchanged made every
  depth-tested draw fail (the black screen). Stencil was missing; the game uses it in up to ~700 draws per frame.
- **Formats:** an unsupported texture format binds an 8x8 white placeholder (k_24_8 depth reads 1.0); cube textures
  bind a black 2D placeholder (objects draw, without reflections).
- **Texture changes:** page watches (unmapping guest pages) fragment the Horizon address space: mapping a page back
  failed with 0xCE01 and crashed. So textures are checked by hashing their guest memory: sampled (4 words per 4 KB),
  at most once per image per frame, and in full for images that changed before (video planes) and for linear
  images. The hash is taken before the copy, so a texture still being written is copied again. Resolve
  destinations are never refreshed from guest memory: the backend never writes it, so the copy there is stale.
- **Predicated tiling:** the game draws 720p as two EDRAM tiles (1280x512 + 1280x208) in gameplay and three 256-row
  strips in cutscenes. In the two-tile layout every draw is issued once, at its final rows (kSinglePassTiles):
  lower-tile duplicates are skipped by a key of their state. The three-strip layout stays as the game draws it:
  each strip has its own post pass (~18 draws) after its first resolve, so a single pass broke the middle strip
  and saved no GPU time.
- **GPU caches:** the frame data ring (vertices, indices, uniforms) is GPU-cached, with an L2 invalidate at the
  start of each frame. Data the GPU writes (timestamps) stays uncached: caching the whole ring hung the game.
- **Render scale:** render targets and resolve destinations are drawn at N/12 of 720p; every guest pixel coordinate
  is scaled where it reaches a host image (no game shader reads gl_FragCoord). Automatic: one step down while the
  smoothed GPU time is over 31 ms (never below 8/12 = 853x480), one step up when the next step is predicted under
  28 ms, at least 60 frames apart. Scaled images live in their own pool, which is reset at every change.
- **Small DXT textures** (64x64 or less: HUD hearts, icons) are decoded to RGBA8 on the CPU. Uploaded compressed,
  the 32x32 DXT5 hearts showed only their left half on the console, although the guest data and our untiling decode
  to a whole heart on the PC and the quad samples the whole texture; depth clamp, no depth test and a fresh upload
  every frame changed nothing. The loss is in the GPU-side copy into a small block-compressed image (2026-11-11).
  Debug flag 21 turns the CPU decode off.
- **Clip disable:** PA_CL_CLIP_CNTL.CLIP_DISABLE turns on depth clamp, as in the SDK's Vulkan path.
- **Render target compression** (DkImageFlags_HwCompression): clears 3.0 to 0.64 ms per frame.
- **Resolves:** repeated resolves of the same picture (the final picture went to the front buffer 4 times per frame)
  are skipped, and back-to-back resolve barriers are merged: 6.6 to 5.3 ms per frame.
- **CPU side:** state caching, a persistent 64 MB vertex/index pool (full hash per frame, sampled re-check on reuse),
  and a shader lookup keyed by guest address and size.

## Shaders

Shaders are translated ahead of time: the SDK's microcode disassembly goes through `tools/scripts/xenos2glsl.py`
(cvhd-recomp's translator, extended) to GLSL, then through uam to DKSH, packed into the NRO's romfs. The backend
looks them up by FNV-1a 64 of the big-endian microcode. `tools/switch-env/stage-shaders.sh` does all of it.

- **Xenos arithmetic (the flicker).** 0 times anything, Inf and NaN included, is 0 on the Xenos for mul, mad, dot
  products and the scalar multiplies; rcpc/rsqc clamp Inf to ±FLT_MAX; rcpf/rsqf flush Inf to 0. The translator had
  plain operations, so zero-length vectors in the lighting shaders (inversesqrt(0) = Inf, then 0 × Inf) gave NaN on
  the Switch GPU: purple and green flicker in the intro, the menu and gameplay. llvmpipe on the PC hid it.
  The exact form is `p = a*b; isnan(p) ? 0 : p`. A taint analysis keeps plain multiplies where no operand can be
  Inf/NaN; `XENOS_EXACT_MULS=1` (the shipped set) makes every multiply exact.
- **Additions to the translator:** the address register (`maxas`, `c[N+a0]`: skinning), `setTexLOD`, the `cube` op,
  vertex texture fetches, unnormalized (texel) fetches, and results written to a register the stage lacks (dropped,
  as the hardware does).
- **uam:** built from source (`tools/uam-src`) with SwitchWaker's three scheduling fixes (uninitialised scheduling
  words, no dual issue, a stall across blocks). They were not the cause of the flicker but are kept.
- **Extraction from the disc** (`tools/shader-extract/`). The shaders sit in GAME.DAT/GAME1.DAT inside LZ2K chunks
  ("LZ2K", unpacked size LE32, packed size LE32; the decoder is written from the public-domain description in
  LunaticInAHat/liblz2k). Consecutive chunks decompress into one buffer that holds D3D shader containers
  (`102A1100` pixel, `102A1101` vertex), with headers and microcode in separate sections, in the same order. A
  control-flow scan finds the microcode (stored length = (last exec end + 1) × 12 bytes) and pairs it with the
  containers in order. `disasm_dump` runs the SDK's own AnalyzeUcode and DumpUcode, so its output is identical to a
  run-time dump.
  Result: 1,546 unique shaders (1,189 pixel, 357 vertex); 143 of the 145 pixel shaders seen in play are among them,
  bit for bit. 1,260 of 1,275 pixel shaders translate; the 15 failures are scan false positives.
- **Generic vertex shaders.** D3D rewrites a vertex shader's vfetch instructions when the game binds it (order,
  format, offset, stride, fetch constant, destination swizzle, all from the vertex declaration; the disc copies have
  stride 0). Each fetch keeps its destination register and write mask, and the rest of the code is identical. So
  the disc copies are named by a key that ignores the fetches: FNV-1a 64 over the microcode with every vfetch
  zeroed, then the sorted (register, mask) pairs (`tools/scripts/vs_generic_key.py`; the backend's GenericVsKey
  gives the same key on all 100 run-time vertex shaders). They are translated with `XENOS_GENERIC_FETCH=1`: each
  fetch writes `rN.c = vsel(in_k, vswz[k].c)`. When the exact shader is missing, the backend loads
  `shader_<key>.gvert/.ggeom.dksh`, maps each bound fetch to input k by (register, mask) from `shader_<key>.gmap`,
  takes the attribute layout from the bound shader, and uploads the swizzles (uniform VFetch, binding 2).
  98 of the 100 vertex shaders seen in play have a disc counterpart. 271 keys, 224 translated: 46 use memexport
  (writes to memory, not supported), 1 has an unknown control-flow form.

## Performance (Switch Lite, handheld clocks: CPU 1020, GPU 460.8, RAM 1331 MHz)

- Typical gameplay about 30 fps with the automatic render scale; the busiest views (2,000+ draws, three-strip
  cutscenes) 16-20 fps. The GPU is the limit in play. On the CPU side the renderer takes 25-33 ms per frame in the
  heaviest cutscenes, so the CPU caps those at about 29 fps.
- Gameplay at native resolution: GPU ~33 ms per frame. Command overhead ~4 ms, resolves ~5 ms, the rest vertex and
  pixel shading. Most expensive shaders: plain lighting covering much of the screen; none pathological.
- sys-clk GPU 768 MHz / RAM 1600 MHz is the largest gain, at the cost of battery life.

## Dead ends

- **Hardware FMZ multiply** (uam `UAM_MUL_ZERO_WINS`, 0 × anything = 0 in hardware): not equivalent to the Xenos;
  the flicker came back.
- **Fast shader math** (`XENOS_FINITE_SPECIAL`: rcp/rsq/log/exp, texels, vertex data and outputs clamped to ±2^60,
  plain multiplies): halves the busiest shaders but still flickered in some scenes, even with the game's float
  constants clamped as they are written (the game writes ~20,000 huge or Inf constants a second), and gained little.
  Kept as `romfs:/shaders_alt1/` (vertex shaders only; debug flag 3, flag 15 for the constant clamp).
- **Single pass for the three-strip layout:** see Predicated tiling.
- **The flicker hunt** before the arithmetic fix ruled out: frame overlap (a full GPU wait after every frame),
  per-frame GPU caches, uam scheduling, torn texture uploads, unmarked shader constants, and a movie decoder
  difference (the WB intro is a real-time 3D scene, not a video).
- **Porting from the PC version:** no recomp toolchain exists for it, and the PC build's renderer is Direct3D 9.

## Debugging tools (development builds: `-DLEGOBATMAN_RELEASE=OFF`)

- **Debug server** (`legobatman/switch/host/debug_server.cpp`, port 6543, no password: local network only). Client:
  `tools/switch-env/lbdbg.py`, console address in `tools/switch-env/switch_host.txt`. Commands:
  - `ping`, `info`, `log` (a 2 MB ring of stdout, then live);
  - `shot` (the next front buffer), `draws` (the next frame's draw list, with each draw's texture fetch constants and first vertices);
  - `skip <PS>` / `unskip`;
  - `press` / `hold` / `release` / `stick` / `trigger`;
  - `get` / `put` / `ls` / `rm` (paths relative to the NRO's folder);
  - `mem <address> <bytes>` (guest physical memory, to decode a texture or buffer on the PC);
  - `flag N V`, the renderer's debug flags:

  | Flag | Effect |
  |---|---|
  | 0 | Re-upload dynamic textures at every bind |
  | 1 | Skip the lower tile (measurement only) |
  | 2 | Two-tile single pass off |
  | 3 N | Use shader set `shaders_altN` |
  | 5 | 1-pixel scissor |
  | 6 | GPU time per category |
  | 7 | Persistent vertex pool off |
  | 8 | Per-descriptor invalidation |
  | 9 | Zero instances |
  | 10 | State cache off |
  | 11 N | Frame log for N frames |
  | 12 | Single pass for the three-strip layout |
  | 13 | The old resolve path |
  | 14 | GPU time per pixel shader |
  | 15 | Clamp the game's float constants |
  | 16 | Clamp diagnostic |
  | 17 N | Render scale: 0 = automatic, 6..12 = fixed twelfths |
  | 18 | Depth clamp on every draw |
  | 19 | No depth test |
  | 20 | Re-upload every texture every frame |
  | 21 | Upload small DXT textures compressed (no CPU decode) |

- **Deploy:** `tools/switch-env/nx-run.sh` sends the NRO to hbmenu's netloader (press Y in hbmenu). The netloader
  writes under `sdmc:/switch/` and strips leading slashes.
- **Frame recorder:** press L3+R3 to record 5 seconds to `sdmc:/switch/legobatman/rec/`; decode with
  `tools/scripts/dkrec.py`.
- **Files next to the NRO:** `nocompress` turns render target compression off; `genericvs` uses the generic vertex
  shaders even where an exact one exists.
- **Linux host:** `legobatman/run.sh` runs the Linux build (`out/linux-nx`) with the SDK's Xenos emulation.
  `legobatman/run-dkhost.sh` runs the deko3d renderer over an OpenGL shim (`-DLEGOBATMAN_DK_GL_HOST=ON`; frames with
  `CVHD_DK_GL_DUMP`, scripted input with `REX_INPUT_SCRIPT`). It does not reproduce NaN behaviour or the uam
  scheduling.
- **Symbolising console addresses:** `aarch64-none-elf-addr2line -f -C -e legobatman/out/switch/legobatman 0x<offset>`
  (inside the build container; offsets are relative to the image).
