# LEGO Batman: The Videogame (Xbox 360) on Nintendo Switch

Static recompilation of the Xbox 360 game for Nintendo Switch (homebrew NRO, libnx + deko3d), built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) through [nfsmw-nx](https://github.com/StevensND/nfsmw-nx)'s
Horizon port of it, with a native deko3d renderer adapted from [cvhd-recomp](https://github.com/rashevskyv/cvhd-recomp).
**No game data is included.** You need your own copy of the Xbox 360 disc (see Game disc).

> **EXPERIMENTAL. READ THIS FIRST.**
>
> - **Performance:** typical gameplay runs at about 30 fps on a stock Switch Lite; the busiest views drop to
>   16-20 fps. The 3D image is drawn at 1280x720 and lowered automatically, down to 853x480, when the GPU falls
>   behind (see Performance).
> - **Very little testing.** The intro, the first story levels and the menus were played, on one console. Most of
>   the game has not been checked.
> - **Flickering and missing graphics are still possible**, and so are **crashes**. Every shader on the disc is
>   translated ahead of time, but the translation of the Xbox 360 GPU's arithmetic is not proven exact everywhere
>   (flicker in some scene), a few shaders could not be extracted or translated, and a shader the port lacks draws
>   nothing (see Missing shaders).
> - Use at your own risk.

## Game disc

The port is built and tested against one release, the disc dumped by Redump as
*LEGO Batman - The Videogame (USA, Europe) (En,Fr,De,Es,It,Da)*:

| | |
|---|---|
| Title ID | 575207D7 |
| Media ID | 191B0669 |
| Discs | 1 of 1 |
| `default.xex` | 3,948,544 bytes, SHA-1 `3902914a108c968c4dfcd4991b756a7ff7ec36da` |

The recompiled code comes from this `default.xex`: another release, region or title update has different code and
will not work. Extract the disc (with an ISO extraction tool such as extract-xiso) and keep its files as they are.

## Install

1. Download `legobatman-<version>-switch.zip` from [Releases](https://github.com/surblazer/legobatman-recomp/releases)
   and unzip it to the root of the SD card.
2. Extract your disc (see Game disc) into `sdmc:/switch/legobatman/game_root/`.

```
sdmc:/switch/legobatman/
    legobatman.nro      the program (from the release, or out/switch-sd/switch/legobatman/ after a build)
    game_root/          the extracted Xbox 360 disc: default.xex, GAME.DAT, GAME1.DAT, Audio/, Movies/
```

The game creates `userdata/` (saves) and an empty `logs/` next to them. Start it from hbmenu (title takeover
recommended: hold R while starting a game, for the full memory).

Log files are off. To turn them on (for a bug report), add `logging = true` at the top of
`sdmc:/switch/legobatman/legobatman.toml` (next to the NRO; create the file if it is not there) and restart the
game: the logs go to `logs/` and `logs/rex/`. A crash writes `logs/rex/rex_crash.log` either way.

## Controls

The game's on-screen prompts show Xbox 360 buttons. By default the face buttons go **by letter**, as in other Switch
games: Switch A is Xbox A (confirm, jump), B is B, X is X, Y is Y.

To use them **by position** instead, as on an Xbox pad (the bottom button is A: Switch B acts as A, A as B, Y as X and
X as Y), add this line at the top of `sdmc:/switch/legobatman/legobatman.toml` (next to the NRO; create the file if
it is not there; above any `[section]` line) and restart the game:

```toml
input_xbox_layout = true
```

`false`, or no line, is the default layout. + is Start and − is Back on both.

## Performance

- **sys-clk:** the port runs at the console's stock clocks (GPU 460.8 MHz in handheld). GPU 768 MHz and RAM
  1600 MHz (the docked clocks) are the biggest gain; on a Switch Lite (Mariko) they are comfortable, at the cost of
  battery life.
- **Status Monitor Overlay** shows FPS and RES (the current 3D resolution) with SaltyNX 0.7.0 or later installed.
- Where the time goes, what was tried and what failed: [PORTING_NOTES.md](PORTING_NOTES.md).

## Missing shaders

The game's shaders are translated ahead of time and packed into the NRO. Every pixel shader is extracted from the
disc's archives (`tools/shader-extract/`). Vertex shaders are rewritten by D3D when the game binds them (their
vertex inputs), so the disc copies are translated as generic shaders that take the input layout from the bound
shader at run time. Both together cover every shader on the disc except 46 vertex shaders that write to memory
(memexport, not supported). An empty file `genericvs` next to the NRO makes the generic vertex shaders replace the
exact ones too (a test).
When the game uses a shader the port lacks, it saves its code to `sdmc:/switch/legobatman/shaders/missing/`; add
those files to `work/shader_dumps_missing/` and rebuild (`tools/switch-env/stage-shaders.sh`, then the build).

## Build

Linux host, podman, about 30 GB of disk.

1. `tools/setup.sh`: nfsmw-nx and uam at their commits with `patches/` applied.
2. The build image: `podman build -t legobatman-switch-env tools/switch-env` (Ubuntu 24.04 + devkitPro).
3. The NVK SDK in `tools/mesa-switch` (nfsmw-nx `docs/mesa.md`), the host SDK build
   (`tools/nfsmw-nx/sdk`, preset `linux-amd64`: the `rexglue` code generator), and `tools/switch-env/build-uam.sh`.
4. Your disc extracted to `game/` (codegen reads `game/default.xex`).
5. `legobatman/codegen.sh`: the recompiled C++ in `legobatman/generated/` (not in the repository).
6. Shaders:
   - pixel shaders from the disc: `tools/shader-extract/build.sh`, then
     `tools/shader-extract/tt_shader_extract work/extracted game/GAME.DAT game/GAME1.DAT` and
     `mkdir work/extracted_ps && cp work/extracted/*.frag work/extracted_ps/` and
     `tools/shader-extract/disasm_dump work/extracted_ps work/shader_dumps_extracted`;
   - vertex shaders from the disc: the same with the `.vert` files into `work/shader_dumps_extracted_vs`;
   - vertex shaders from play: dumps in `work/shader_dumps/` (the SDK's `--dump_shaders` on a Linux run) and
     `work/shader_dumps_missing/` (from the console);
   - then `tools/switch-env/stage-shaders.sh`.
7. `tools/switch-env/build-switch.sh`: `legobatman/out/switch/legobatman.nro`, copied to
   `legobatman/out/switch-sd/switch/legobatman/`.

The hbmenu icon is `legobatman/icon.jpg` (256x256, box art from SteamGridDB); without it the default icon is used.

Development builds (`-DLEGOBATMAN_RELEASE=OFF`) add a debug server on port 6543 (no password: local network only)
for logs, screenshots, input and runtime switches (`tools/switch-env/lbdbg.py`), and push to hbmenu's netloader
with `tools/switch-env/nx-run.sh`.

## Layout

| Path | What |
|---|---|
| `legobatman/` | The port: CMake project, codegen manifest and overrides, `src/` (app, renderer `src/gpu/`), `switch/` |
| `legobatman/src/gpu/deko3d_backend.cpp` | The native renderer behind the SDK's Xenos command processor |
| `tools/scripts/xenos2glsl.py` | Xenos shader microcode to GLSL (then uam to DKSH) |
| `tools/shader-extract/` | Offline shader extraction from GAME.DAT (LZ2K decoder, microcode finder, SDK disassembly) |
| `tools/switch-env/` | Build image, build and staging scripts, debug client |
| `patches/` | Our changes to nfsmw-nx's SDK and uam (`tools/setup.sh` applies them) |
| `PORTING_NOTES.md` | The porting journal: findings, measurements, dead ends |

## Credits

- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (Tom Clay; derived from [Xenia](https://github.com/xenia-project/xenia))
- [nfsmw-nx](https://github.com/StevensND/nfsmw-nx): the Horizon port of the SDK and its documentation
- [cvhd-recomp](https://github.com/rashevskyv/cvhd-recomp) (rashevskyv): the deko3d renderer, shader translator and
  frame recorder this port started from
- [uam](https://github.com/devkitPro/uam) (fincs, devkitPro) and SwitchWaker's scheduling patches
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (hedge-dev): XenonAnalyse, for the first analysis of the executable

## Legal

LEGO Batman: The Videogame is © TT Games, Warner Bros. Interactive Entertainment, The LEGO Group and DC Comics.
This project is not affiliated with them.
