// legobatman - ReXGlue Recompiled Project
//
// App customisation shared by the Linux and Switch builds:
//  - GPU emulation (xenos) on by default, so the game starts without arguments.
//  - Default paths next to the executable, which on the Switch is sdmc:/switch/legobatman/:
//      game_root/   extracted disc (default.xex, GAME.DAT, GAME1.DAT, Audio/, Movies/)
//      userdata/    saves and caches, kept apart from the disc files
//    Command-line --game_data_root / --user_data_root still take precedence.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/system/gpu_plugin.h>
#if REX_PLATFORM_SWITCH
#include <rex/ui/switch_apm.h>
extern "C" const char* RexSwitchLogDir(void);  // SDK switch_crash_hooks.c: "<NRO folder>/logs/rex/"
extern "C" void RexSwitchPerfReports(int on);  // SDK switch_perf.cpp: the 10 s report to rex_profile.log
extern "C" int RexSwitchLogFiles(void);        // main.cpp: legobatman.toml logging (release default off)
#ifndef LEGOBATMAN_RELEASE
extern "C" void lb_dbg_start(const char* root, int port);  // switch/host/debug_server.cpp
#endif
#endif
#include <cstdio>
#include <string>

#include <filesystem>
#include <system_error>

// Backend inside the GPU plugin: "any" (the SDK's Vulkan/Xenos emulation), "trace" (state census, nothing drawn),
// "deko3d" (native renderer; on Linux only in a -DLEGOBATMAN_DK_GL_HOST=ON build). After cvhd-recomp.
// Read before the config loader (main.cpp, RexSwitchLogFiles); declared so the key is a known setting.
REXCVAR_DEFINE_BOOL(logging, false, "Logging", "Write log files to the SD card (Switch; default on in development builds)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(gpu_backend, "any", "GPU", "Backend inside the GPU plugin: any, vulkan, trace, deko3d")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

class LegobatmanApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<LegobatmanApp>(new LegobatmanApp(ctx, "legobatman",
        PPCImageConfig));
  }

  void OnPostInitLogging() override {
    // Without a GPU plugin the game runs but nothing is drawn. gpu_plugin is read right after this hook.
    SetIfDefault("gpu_plugin", "xenos");
#if REX_PLATFORM_SWITCH
    // Handheld GPU at 460.8 MHz with memory unchanged (performance configuration 0x92220008, the pair retail games
    // use; not an overclock). Default handheld is 307.2 MHz. No effect when docked or if the firmware lacks it.
    RexSwitchApmPedirGpuMhz(460);
    RexSwitchApmAplicar();
#endif
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Load the GPU plugin here instead of letting SetupPresentation do it right after this hook: its cvars only
    // exist once it is loaded, and some must be set before the GPU initialises.
    if (!config.graphics && !config.gpu_plugin.empty()) {
      const std::string backend = REXCVAR_GET(gpu_backend);
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
      REXLOG_INFO("GPU: plugin '{}', backend '{}' -> {}", config.gpu_plugin, backend,
                  config.graphics ? "loaded" : "FAILED (falling back to the plugin's default)");
      if (!config.graphics) {
        return;  // SetupPresentation retries and reports the error
      }
    }
    // Pixel-accurate EDRAM emulation (fragment shader interlock). The default host render-target path left the
    // second Chapter 1 intro cutscene black. NVK exposes interlock on Maxwell B..Pascal, so the Switch's
    // Tegra X1 (GM20B) supports it too; its GPU cost on the console is not measured yet.
    SetIfDefault("render_target_path_vulkan", "fsi");
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
#if REX_PLATFORM_SWITCH
    // stdout -> <NRO folder>/logs/rex/rex_stdout.log, line-buffered. The SDK redirects only stderr; the deko3d
    // backend ([dk] lines), the memory layer ([gm]) and cvhd-style diagnostics print to stdout, which was lost.
#ifndef LEGOBATMAN_RELEASE
    if (RexSwitchLogFiles()) {
      const std::string out = std::string(RexSwitchLogDir()) + "rex_stdout.log";
      if (std::freopen(out.c_str(), "w", stdout)) {
        std::setvbuf(stdout, nullptr, _IOLBF, 0);
      }
    }
#endif
    // The debug server (switch/host/debug_server.cpp): log, screenshots, input and SD files for the development PC on
    // port 6543. Development builds only: no password, local network.
#ifndef LEGOBATMAN_RELEASE
    {
      const auto exe_dir = rex::filesystem::GetExecutableFolder().string();
      lb_dbg_start(exe_dir.c_str(), 6543);
    }
#else
    // Release: the SDK's stack sampler (switch_perf.cpp) pauses every thread each millisecond while
    // logs/rex/profile_stacks.flag exists, a diagnostics file left by development builds: removed before the sampler
    // looks for it (it waits for the game to start first).
    std::remove((std::string(RexSwitchLogDir()) + "profile_stacks.flag").c_str());
#endif
    // No log files unless legobatman.toml says logging = true (release builds; development builds default to
    // true): the game log off (no file is created at that level), no performance report. Kept: the crash report (on
    // a crash only) and the missing-shader dumps.
    if (!RexSwitchLogFiles()) {
      SetIfDefault("log_level", "off");
      RexSwitchPerfReports(0);
    }
    // Write the log from its own thread: with the default flush-on-warning, every warning was written to the SD
    // card synchronously by the logging thread (~40% of GPU-command/vsync thread samples). This hook runs before
    // the config file is loaded and before logging starts, as init-only cvars require.
    SetIfDefault("log_async", "true");
#endif
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) {
      return;
    }
    std::error_code ec;
    if (paths.game_data_root.empty()) {
      const auto game = folder / "game_root";
      if (std::filesystem::is_directory(game, ec)) {
        paths.game_data_root = game;
      }
      // If missing, the SDK reports that no game data was found.
    }
    if (paths.user_data_root.empty()) {
      const auto user = folder / "userdata";
      std::filesystem::create_directories(user, ec);
      if (!ec) {
        paths.user_data_root = user;
      }
    }
  }

 private:
  // Sets a cvar only when nobody chose a value (command line or config file win).
  static void SetIfDefault(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) == nullptr || rex::cvar::HasNonDefaultValue(name)) {
      return;
    }
    if (rex::cvar::SetFlagByName(name, value)) {
      REXLOG_DEBUG("default setting {} = {}", name, value);
    }
  }
};
