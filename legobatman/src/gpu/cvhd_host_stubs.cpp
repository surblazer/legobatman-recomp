// LEGO Batman port: the cvhd-recomp Switch host-layer symbols the deko3d backend links against
// (cvhd's switch/host/switch_nx.cpp, nx_menu.cpp, frame_recorder.cpp and an SDK vblank-divider edit).
// This port runs on nfsmw-nx's SDK and has none of those, so they are inert stand-ins:
//  - counters the backend writes for cvhd's busy/frame log line,
//  - the port settings menu (fps mode 60, fps counter on, no overlay); the recorder flag is real (L3+R3 in the
//    backend sets it) and the recorder itself is cvhd's frame_recorder.cpp (switch/host/),
//  - the dirty-page arrays: unused here, the backend runs in its hashing mode (LEGOBATMAN_HASH_TEXTURES).
#include <cstdint>
#include <cstring>

#include "../../switch/host/include/rex/frame_recorder.h"
#include "../../switch/host/include/rex/nx_menu.h"

extern "C" {
volatile unsigned rex_nx_swaps = 0;
volatile double rex_nx_main_busy_ms = 0;
volatile int rex_nx_vblank_divider = 1;  // cvhd's SDK divides guest vblank interrupts by this; ours does not
volatile uint8_t rex_nx_dirty[0x20000] = {};
volatile uint8_t rex_nx_dirty_coarse[0x200] = {};

void rex_nx_waited_add(unsigned long long) {}
int rex_nx_setting_fps_mode(void) { return 0; }
int rex_nx_setting_fps_counter(void) { return 1; }
static volatile int g_record = 0;
int rex_nx_debug_record(void) { return g_record; }
void rex_nx_debug_record_set(int on) { g_record = on; }
int rex_nx_menu_overlay(NxOverlay* out, const char*, int, int) {
  std::memset(out, 0, sizeof(*out));
  return 0;
}
}

