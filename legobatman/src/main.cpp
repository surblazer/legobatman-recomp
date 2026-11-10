// legobatman - ReXGlue Recompiled Project

#include "generated/default/legobatman_init.h"

#include "legobatman_app.h"

REX_DEFINE_APP(legobatman, LegobatmanApp::Create)

#if REX_PLATFORM_SWITCH
#include <cctype>
#include <cstdio>
#include <cstring>

extern int __system_argc;     // libnx (argv.c), filled in before the constructors
extern char** __system_argv;

// Log files on the SD card (the SDK's rex_stderr.log and rex_profile.log, the game log, rex_stdout.log): key
// `logging = true|false` in legobatman.toml next to the NRO. Default: off in release builds, on in development
// builds. The SDK asks before its first file, from a constructor, so the key is read here, before the config loader
// runs. Crash and assert logs are written whatever the setting, when they happen.
extern "C" int RexSwitchLogFiles(void) {
  static int cached = -1;
  if (cached >= 0) return cached;
#ifdef LEGOBATMAN_RELEASE
  cached = 0;
#else
  cached = 1;
#endif
  if (__system_argc < 1 || !__system_argv || !__system_argv[0]) return cached;
  const char* nro = __system_argv[0];
  const char* slash = std::strrchr(nro, '/');
  if (!slash) return cached;
  char path[768];
  std::snprintf(path, sizeof(path), "%s%.*slegobatman.toml", std::strchr(nro, ':') ? "" : "sdmc:",
                int(slash - nro + 1), nro);
  FILE* f = std::fopen(path, "r");
  if (!f) return cached;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char* p = line;
    while (std::isspace(static_cast<unsigned char>(*p))) p++;
    if (*p == '[') break;  // top-level keys only
    if (std::strncmp(p, "logging", 7) != 0) continue;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '=') continue;
    while (*p == ' ' || *p == '\t') p++;
    if (std::strncmp(p, "true", 4) == 0) cached = 1;
    else if (std::strncmp(p, "false", 5) == 0) cached = 0;
  }
  std::fclose(f);
  return cached;
}
#endif
