// Port menu RECORD 5 SEC (L3+R3): every drawn frame's front buffer for 5 s, lossless, in one file, plus one log line
// per frame with 16x9 tile hashes (a flicker shows as a tile hash that changes while the game is paused).
//
// File <dir>/rec_<first swap>.hrec: per frame a 32-byte header {"HREC", index, swap, microseconds since the start,
// width, height, compressed bytes, 0} and the frame XOR the previous one (the first one XOR zero), LZ4 block compressed:
// a still picture costs almost nothing. tools/dkrec.py turns it into PNGs, a video, a contact sheet and change maps.
// The backend copies the frame (GPU) and hands it over once the frame's fence has passed; the writer thread does the
// hashes, the XOR and the compression into a RAM arena, and the file is written after the 5 s: the SD card took only
// part of the 50+ MB/s of compressed frames (Contra 0.264: 68-81 of about 170 frames dropped; cvhd records the 1920x1080 guest front buffer: 2.25x the work, drops are counted in the stop line). A full ring or a full
// arena drops a frame (counted in the log).
#include "include/rex/frame_recorder.h"  // LEGO Batman port: from cvhd-recomp (BSD-3), unchanged otherwise

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <lz4.h>
#include <sys/stat.h>
#include <switch.h>

namespace {

constexpr int kRing = 16;  // frames waiting for the compressor (16 x 3.6 MB)

struct Pending {
  std::vector<uint8_t> px;
  uint32_t swap = 0;
  uint64_t us = 0;
};

std::mutex g_lock;
std::condition_variable g_cv;
std::deque<Pending*> g_queue, g_free;
std::thread g_thread;
bool g_active = false, g_stop = false;
uint32_t g_w = 0, g_h = 0, g_pushed = 0, g_dropped = 0;
uint8_t* g_arena = nullptr;  // the compressed frames until the file is written
size_t g_arena_size = 0;

uint16_t TileHash(const uint8_t* px, uint32_t w, uint32_t x0, uint32_t y0, uint32_t tw, uint32_t th) {
  uint32_t h = 2166136261u;
  for (uint32_t y = y0; y < y0 + th; y++) {
    const uint32_t* row = reinterpret_cast<const uint32_t*>(px) + size_t(y) * w + x0;
    for (uint32_t x = 0; x < tw; x++) h = (h ^ row[x]) * 16777619u;
  }
  return uint16_t(h ^ (h >> 16));
}

void Writer(std::string path) {
  const size_t bytes = size_t(g_w) * g_h * 4;
  size_t used = 0;
  uint32_t arena_full = 0;
  uint64_t busy_ticks = 0;
  std::vector<uint8_t> prev(bytes, 0), delta(bytes), out(static_cast<size_t>(LZ4_compressBound(int(bytes))));
  uint32_t index = 0;
  uint64_t written = 0;
  for (;;) {
    Pending* p;
    {
      std::unique_lock<std::mutex> l(g_lock);
      g_cv.wait(l, [] { return !g_queue.empty() || g_stop; });
      if (g_queue.empty()) break;
      p = g_queue.front();
      g_queue.pop_front();
    }
    const uint64_t t0 = armGetSystemTick();
    // 16x9 tiles of 80x80 pixels (1280x720): one 16-bit hash each, and one for the frame.
    char line[16 * 9 * 5 + 96];
    int n = std::snprintf(line, sizeof(line), "[rec] frame %u swap %u t %llu ms:", index, p->swap, (unsigned long long)(p->us / 1000));
    const uint32_t tw = g_w / 16, th = g_h / 9;
    for (uint32_t ty = 0; ty < 9; ty++)
      for (uint32_t tx = 0; tx < 16; tx++) n += std::snprintf(line + n, sizeof(line) - n, " %04X", TileHash(p->px.data(), g_w, tx * tw, ty * th, tw, th));
    std::printf("%s\n", line);
    for (size_t i = 0; i < bytes; i++) delta[i] = p->px[i] ^ prev[i];
    prev.swap(p->px);
    const int c = LZ4_compress_default(reinterpret_cast<const char*>(delta.data()), reinterpret_cast<char*>(out.data()), int(bytes), int(out.size()));
    if (c > 0 && used + 32 + size_t(c) <= g_arena_size) {
      const uint32_t hdr[8] = {0x43455248u /* "HREC" */, index, p->swap, uint32_t(p->us), g_w, g_h, uint32_t(c), 0};
      std::memcpy(g_arena + used, hdr, sizeof(hdr));
      std::memcpy(g_arena + used + 32, out.data(), size_t(c));
      used += 32 + size_t(c);
      index++;
    } else {
      arena_full++;
      prev.swap(p->px);  // undo: the next frame is XORed with the last one stored
    }
    busy_ticks += armGetSystemTick() - t0;
    std::lock_guard<std::mutex> l(g_lock);
    g_free.push_back(p);
  }
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(g_arena, 1, used, f);
    std::fclose(f);
    written = used;
  }
  std::printf("[rec] stop: %u frames written (%llu KB) to %s; %u dropped (ring full: the compressor was behind), %u dropped (arena of %zu MB "
              "full); compressor %.1f ms per frame\n",
              index, (unsigned long long)(written >> 10), path.c_str(), g_dropped, arena_full, g_arena_size >> 20,
              index + arena_full ? armTicksToNs(busy_ticks) / 1e6 / (index + arena_full) : 0.0);
  std::fflush(stdout);
}

}  // namespace

bool rex_rec_start(const char* dir, uint32_t first_swap, uint32_t w, uint32_t h) {
  if (g_active) return true;
  mkdir(dir, 0777);
  g_w = w;
  g_h = h;
  g_pushed = g_dropped = 0;
  g_stop = false;
  try {
    for (int i = 0; i < kRing; i++) {
      auto* p = new Pending;
      p->px.resize(size_t(w) * h * 4);
      g_free.push_back(p);
    }
  } catch (...) {
    std::printf("[rec] not started: no memory for %d frames\n", kRing);
    for (Pending* p : g_free) delete p;
    g_free.clear();
    return false;
  }
  for (size_t mb : {320, 192, 128, 64}) {
    g_arena = static_cast<uint8_t*>(std::malloc(mb << 20));
    if (g_arena) { g_arena_size = mb << 20; break; }
  }
  if (!g_arena) {
    std::printf("[rec] not started: no memory for the arena\n");
    for (Pending* p : g_free) delete p;
    g_free.clear();
    return false;
  }
  char path[256];
  std::snprintf(path, sizeof(path), "%srec_%u.hrec", dir, first_swap);
  std::printf("[rec] start at swap %u: %ux%u, 5 s, to %s (arena %zu MB)\n", first_swap, w, h, path, g_arena_size >> 20);
  g_thread = std::thread(Writer, std::string(path));
  g_active = true;
  return true;
}

bool rex_rec_active() { return g_active; }

void rex_rec_push(const uint8_t* rgba, uint32_t swap, uint64_t us) {
  Pending* p = nullptr;
  {
    std::lock_guard<std::mutex> l(g_lock);
    if (!g_active || g_free.empty()) {
      if (g_active) g_dropped++;
      return;
    }
    p = g_free.front();
    g_free.pop_front();
  }
  std::memcpy(p->px.data(), rgba, p->px.size());
  p->swap = swap;
  p->us = us;
  std::lock_guard<std::mutex> l(g_lock);
  g_queue.push_back(p);
  g_pushed++;
  g_cv.notify_one();
}

void rex_rec_stop() {
  if (!g_active) return;
  {
    std::lock_guard<std::mutex> l(g_lock);
    g_stop = true;
    g_cv.notify_one();
  }
  g_thread.join();
  for (Pending* p : g_free) delete p;
  g_free.clear();
  std::free(g_arena);
  g_arena = nullptr;
  g_active = false;
}
