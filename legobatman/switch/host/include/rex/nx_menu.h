// In-game settings menu and the settings file (sdmc:/switch/cvhd/settings.txt), nx_menu.cpp.
// The pad driver feeds every poll (minus opens and closes the menu; while it is open the game gets no
// input), the present pass draws the overlay it describes, the backend / pad / audio read the settings.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int rex_nx_setting_fps_mode(void);     // 0 = 60, 1 = 30, 2 = 30 skip
int rex_nx_setting_rumble(void);       // 0 / 1
int rex_nx_setting_volume(void);       // 0-100
int rex_nx_setting_fps_counter(void);  // 0 / 1
int rex_nx_setting_button_icons(void); // 0 = the game's Xbox glyphs, 1 = swapped for the Switch (nx_glyphs.cpp)
int rex_nx_debug_record(void);         // 1 = a 5 s recording asked (the file record_now; the backend sets it back to 0 when done)
void rex_nx_debug_record_set(int on);

// buttons = the 360 button mask of the poll (X_INPUT_GAMEPAD_*), lx/ly = the left stick. Returns 1 while
// the menu is open: the caller then reports an idle pad to the game.
int rex_nx_menu_input(uint16_t buttons, int16_t lx, int16_t ly);

// The text grid the present pass draws (present.frag "Hud"): cols x rows cells of 6x8 glyph units at
// `scale` pixels per unit, the top-left at (x, y) output pixels, a dark box behind it when `box`.
// cells[2i], cells[2i+1] = the 5x7 glyph of cell i: rows 0-3 as 20 bits, rows 4-6 as 15 bits (row-major,
// the left column the high bit of each 5-bit row), as floats for the shader.
#define NX_OVERLAY_COLS 48
#define NX_OVERLAY_ROWS 16
typedef struct NxOverlay {
  int cols, rows, scale, x, y, box;
  float row_rgb[NX_OVERLAY_ROWS][4];  // text colour per row; w = 1 marks the highlighted row
  float cells[NX_OVERLAY_ROWS * NX_OVERLAY_COLS * 2];
} NxOverlay;

// Fills `out` with the menu when it is open, else with the fps line (empty = nothing). Returns 1 when
// there is something to draw.
int rex_nx_menu_overlay(NxOverlay* out, const char* fps_text, int out_w, int out_h);

#ifdef __cplusplus
}
#endif
