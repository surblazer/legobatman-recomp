# legobatman - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(legobatman LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GPU backends compiled into the SDK's GPU plugin (after cvhd-recomp, src/gpu/UPSTREAM.txt): the trace backend
# (--gpu_backend=trace: every distinct draw/resolve/swap state logged, nothing drawn) everywhere; the deko3d renderer
# on the Switch; on Linux the same renderer over an OpenGL stand-in for libdeko3d with -DLEGOBATMAN_DK_GL_HOST=ON.
option(LEGOBATMAN_DK_GL_HOST "Linux: the deko3d backend over OpenGL (test host)" OFF)
set(REXGPU_XENOS_EXTRA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/trace_backend.cpp")
if(REXGLUE_PLATFORM_SWITCH)
    list(APPEND REXGPU_XENOS_EXTRA_SOURCES
         "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/deko3d_backend.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/cvhd_host_stubs.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/switch/host/frame_recorder.cpp")  # RECORD 5 SEC (L3+R3), from cvhd
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LEGOBATMAN_DK_GL_HOST)
    find_path(LEGOBATMAN_EGL_INCLUDE EGL/egl.h REQUIRED)
    list(APPEND REXGPU_XENOS_EXTRA_SOURCES
         "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/deko3d_backend.cpp"
         "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/dk_gl/dk_gl_shim.cpp")
endif()

include(generated/rexglue.cmake)

# Sources
set(LEGOBATMAN_SOURCES
    src/main.cpp
    src/legobatman_waits.cpp
)
# Release build (default): no debug server (it has no password and accepts files, input and commands from the
# network), no L3+R3 frame recorder, and the SDK's stack sampler kept off. -DLEGOBATMAN_RELEASE=OFF for development.
option(LEGOBATMAN_RELEASE "Switch release build: no debug server or frame recorder" ON)
if(REXGLUE_PLATFORM_SWITCH AND NOT LEGOBATMAN_RELEASE)
    list(APPEND LEGOBATMAN_SOURCES switch/host/debug_server.cpp)  # the debug server (log, shot, input, files)
endif()

if(WIN32)
    add_executable(legobatman WIN32 ${LEGOBATMAN_SOURCES})
else()
    add_executable(legobatman ${LEGOBATMAN_SOURCES})
endif()

rexglue_setup_target(legobatman GPU_PLUGINS xenos)
if(TARGET rexgpu-xenos AND REXGLUE_PLATFORM_SWITCH)
    target_link_libraries(rexgpu-xenos PRIVATE deko3d lz4)   # libdeko3d.a from libnx; lz4 for the frame recorder
endif()
if(LEGOBATMAN_RELEASE AND REXGLUE_PLATFORM_SWITCH)
    target_compile_definitions(legobatman PRIVATE LEGOBATMAN_RELEASE=1)
    if(TARGET rexgpu-xenos)
        target_compile_definitions(rexgpu-xenos PRIVATE LEGOBATMAN_RELEASE=1)
    endif()
endif()
if(TARGET rexgpu-xenos AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND LEGOBATMAN_DK_GL_HOST)
    target_link_libraries(rexgpu-xenos PRIVATE EGL GL)
endif()

# rex_app.h includes imgui.h, but rexruntime links imgui privately, so its include path does not reach the
# executable. On Switch rexglue_switch.cmake adds it; elsewhere take the path alone (as nfsmw-nx does).
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(legobatman PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Nintendo Switch: legobatman.nro next to the ELF. On the SD card it goes in sdmc:/switch/legobatman/
# with the extracted disc in game_root/ beside it (src/legobatman_app.h looks for it there).
# Exact PowerPC arithmetic for the game code, as the SDK's own targets get (its add_compile_options does not reach
# this directory): no FP contraction (GCC fuses a*b+c into FMA on aarch64 and the results drift from the PPC: the
# intro movie's decoder drifted to wrong colours and broken blocks on the console only) and no strict-aliasing
# assumptions (the recompiled code type-puns memory everywhere). With LTO the flags must also reach the link.
foreach(tgt legobatman_recomp legobatman)
    if(TARGET ${tgt})
        target_compile_options(${tgt} PRIVATE -fno-strict-aliasing
            $<IF:$<CXX_COMPILER_ID:GNU>,-ffp-contract=off,-ffp-model=strict>)
    endif()
endforeach()
if(REXGLUE_PLATFORM_SWITCH)
    target_link_options(legobatman PRIVATE -fno-strict-aliasing -ffp-contract=off)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # LTO on the game code and the app (not the SDK libraries), as nfsmw-nx does. With the direct-call rewrite
    # (tools/scripts/direct_calls.py) GCC can inline small game functions across files. -flto=10 with balanced
    # partitions keeps the whole-program step within memory for ~120 MB of generated source.
    option(LEGOBATMAN_LTO "LTO for the game code and app (Switch)" ON)
    if(LEGOBATMAN_LTO)
        foreach(tgt legobatman_recomp legobatman)
            if(TARGET ${tgt})
                target_compile_options(${tgt} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(legobatman PRIVATE -flto=10 -flto-partition=balanced)  # 16 cores, 31 GB: 10 LTO jobs (was 2)
    endif()
    # icon.jpg: the box-art icon from SteamGridDB (game 1996, icon 0bbb2ee3...), 512x512 scaled to 256x256.
    # Not in the repository (cover art): without it the build uses devkitPro's default icon.
    set(LEGOBATMAN_ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg")
    if(NOT EXISTS "${LEGOBATMAN_ICON}")
        set(LEGOBATMAN_ICON "${DEVKITPRO}/libnx/default_icon.jpg")
    endif()
    rexglue_switch_add_nro(legobatman
        NAME "LEGO Batman"
        AUTHOR "legobatman-nx"
        VERSION "0.1.0"
        ICON "${LEGOBATMAN_ICON}")
    # The DKSH shaders in the NRO's romfs (romfs:/shaders/), so a netloader deploy carries them: the NRO is packaged a
    # second time with --romfsdir (tools/switch-env/stage-shaders.sh fills out/romfs/shaders/).
    set(LEGOBATMAN_ROMFS "${CMAKE_CURRENT_SOURCE_DIR}/out/romfs")
    if(EXISTS "${LEGOBATMAN_ROMFS}/shaders")
        find_program(LEGOBATMAN_ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)
        add_custom_command(TARGET legobatman POST_BUILD
            COMMAND "${LEGOBATMAN_ELF2NRO}" "$<TARGET_FILE_DIR:legobatman>/legobatman.stripped.elf"
                "$<TARGET_FILE_DIR:legobatman>/legobatman.nro" "--nacp=$<TARGET_FILE_DIR:legobatman>/legobatman.nacp"
                "--icon=${LEGOBATMAN_ICON}" "--romfsdir=${LEGOBATMAN_ROMFS}"
            COMMENT "Packaging legobatman.nro with the shaders in romfs"
            VERBATIM)
    endif()
    # A ready-to-copy SD card layout: out/switch-sd/switch/legobatman/legobatman.nro (the game data goes in game_root/
    # next to it on the card). Runs after the packaging steps above, so it copies the final NRO.
    set(LEGOBATMAN_SD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/out/switch-sd/switch/legobatman")
    add_custom_command(TARGET legobatman POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${LEGOBATMAN_SD_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE_DIR:legobatman>/legobatman.nro" "${LEGOBATMAN_SD_DIR}/legobatman.nro"
        COMMENT "Copying legobatman.nro to out/switch-sd/switch/legobatman/"
        VERBATIM)
endif()
