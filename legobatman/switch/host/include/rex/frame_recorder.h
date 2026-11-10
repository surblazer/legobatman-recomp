// Port menu RECORD 5 SEC: see frame_recorder.cpp.
#pragma once

#include <stdint.h>

// dir ends with '/'. False = not started (no memory).
bool rex_rec_start(const char* dir, uint32_t first_swap, uint32_t w, uint32_t h);
bool rex_rec_active();
// One frame, w x h RGBA8 (copied at once). A full ring drops it.
void rex_rec_push(const uint8_t* rgba, uint32_t swap, uint64_t us);
// The writer finishes the queued frames, then the file is closed (blocks until then).
void rex_rec_stop();
