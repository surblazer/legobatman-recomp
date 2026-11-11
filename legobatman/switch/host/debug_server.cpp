// LEGO Batman port: debug server (Switch only). A TCP port on the local network for the development PC: the log as it is
// written, the next presented frame as RGBA, controller input, SD card files. The protocol follows SwitchWaker's
// (centollOS/SwitchWaker docs/DEBUG_SERVER.md, MIT): plain TCP, one command per line, every reply "ok <n>\n" or
// "err <n>\n" followed by n bytes. No password: local network only. The client is tools/switch-env/lbdbg.py.
//
//   ping | info | log | shot | press BTN[+BTN..] [ms] | hold BTN[+..] | release [BTN[+..]] | stick L|R x y |
//   trigger L|R v | get PATH | put PATH SIZE (then SIZE bytes) | ls PATH | rm PATH
// Paths are relative to the NRO's folder (sdmc:/switch/legobatman/) unless they start with '/'.
#include <switch.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
// Input override, read by the SDK's InputSystem::GetState (tools/nfsmw-nx/sdk/src/input/input_system.cpp).
extern volatile int lb_dbg_input_on;
extern volatile uint16_t lb_dbg_buttons;
extern volatile int16_t lb_dbg_lx, lb_dbg_ly, lb_dbg_rx, lb_dbg_ry;
extern volatile uint8_t lb_dbg_lt, lb_dbg_rt;
extern volatile unsigned rex_nx_swaps;
}
// Screenshot, served by the deko3d backend at its next swap (src/gpu/deko3d_backend.cpp).
bool lb_dbg_shot(std::vector<uint8_t>* rgba, uint32_t* w, uint32_t* h, int timeout_ms);
void lb_dbg_skip(uint64_t ps, bool on);
bool lb_dbg_mem(uint32_t address, uint32_t bytes, std::string* out);
void lb_dbg_flag(int n, int v);
bool lb_dbg_draws(std::vector<std::string>* out, int timeout_ms);

namespace {

std::string g_root = "sdmc:/switch/legobatman/";

// ----- the log: stdout goes to its file and to a ring kept in memory -------------------------------------------
constexpr size_t kRing = 2u << 20;
std::mutex g_log_lock;
std::condition_variable g_log_cv;
std::vector<char> g_ring(kRing);
uint64_t g_written = 0;  // total bytes ever written (the ring holds the last kRing)
// stdout stays the freopen'ed file (one FILE object shared by every thread); its low-level write function is wrapped
// so each flushed chunk also lands in the ring.
using WriteFn = decltype(stdout->_write);
WriteFn g_orig_write = nullptr;

_ssize_t TeeWrite(struct _reent* r, void* cookie, const char* buf, size_t n) {
  const _ssize_t k = g_orig_write ? g_orig_write(r, cookie, buf, n) : _ssize_t(n);
  if (n > 0) {
    {
      std::lock_guard<std::mutex> lk(g_log_lock);
      for (size_t i = 0; i < n; i++) g_ring[(g_written + uint64_t(i)) % kRing] = buf[i];
      g_written += uint64_t(n);
    }
    g_log_cv.notify_all();
  }
  return k;
}

// ----- sockets ---------------------------------------------------------------------------------------------------
bool SendAll(int fd, const void* data, size_t n) {
  const char* p = static_cast<const char*>(data);
  while (n) {
    ssize_t k = send(fd, p, n, 0);
    if (k <= 0) return false;
    p += k;
    n -= size_t(k);
  }
  return true;
}
bool Reply(int fd, bool ok, const std::string& body) {
  char head[32];
  int h = std::snprintf(head, sizeof(head), "%s %zu\n", ok ? "ok" : "err", body.size());
  return SendAll(fd, head, size_t(h)) && SendAll(fd, body.data(), body.size());
}
bool RecvLine(int fd, std::string& line) {
  line.clear();
  char c;
  while (true) {
    ssize_t k = recv(fd, &c, 1, 0);
    if (k <= 0) return false;
    if (c == '\n') return true;
    if (c != '\r') line += c;
    if (line.size() > 4096) return false;
  }
}
bool RecvAll(int fd, char* p, size_t n) {
  while (n) {
    ssize_t k = recv(fd, p, n, 0);
    if (k <= 0) return false;
    p += k;
    n -= size_t(k);
  }
  return true;
}
std::vector<std::string> Split(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : s) {
    if (c == '"') { quoted = !quoted; continue; }
    if (c == ' ' && !quoted) { if (!cur.empty()) out.push_back(cur); cur.clear(); continue; }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
std::string PathOf(const std::string& p) { return p.size() && p[0] == '/' ? "sdmc:" + p : g_root + p; }

// Xbox 360 button bits (the game sees an Xbox pad; Switch A/B are mapped by the SDK's input layer).
uint16_t Buttons(const std::string& spec, bool* ok) {
  static const struct { const char* n; uint16_t b; } kNames[] = {
      {"UP", 0x1}, {"DOWN", 0x2}, {"LEFT", 0x4}, {"RIGHT", 0x8}, {"START", 0x10}, {"BACK", 0x20}, {"LS", 0x40},
      {"RS", 0x80}, {"LB", 0x100}, {"RB", 0x200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000}};
  uint16_t bits = 0;
  size_t pos = 0;
  *ok = true;
  while (pos <= spec.size()) {
    size_t end = spec.find('+', pos);
    if (end == std::string::npos) end = spec.size();
    std::string n = spec.substr(pos, end - pos);
    for (auto& ch : n) ch = char(toupper(ch));
    bool found = false;
    for (auto& e : kNames) if (n == e.n) { bits |= e.b; found = true; }
    if (!found && !n.empty()) *ok = false;
    pos = end + 1;
  }
  return bits;
}

void Client(int fd) {
  std::string line;
  while (RecvLine(fd, line)) {
    auto a = Split(line);
    if (a.empty()) continue;
    const std::string& cmd = a[0];
    if (cmd == "ping") {
      if (!Reply(fd, true, "pong")) break;
    } else if (cmd == "info") {
      u64 total = 0, used = 0;
      svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
      char b[256];
      std::snprintf(b, sizeof(b), "legobatman debug server\nswap %u\nmemory %llu/%llu MB\nlog bytes %llu\n", rex_nx_swaps,
                    (unsigned long long)(used >> 20), (unsigned long long)(total >> 20), (unsigned long long)g_written);
      if (!Reply(fd, true, b)) break;
    } else if (cmd == "log") {
      // The kept text (up to 2 MB), then every new byte until the client closes.
      if (!Reply(fd, true, "")) break;
      uint64_t pos;
      {
        std::lock_guard<std::mutex> lk(g_log_lock);
        pos = g_written > kRing ? g_written - kRing : 0;
      }
      std::vector<char> chunk;
      while (true) {
        {
          std::unique_lock<std::mutex> lk(g_log_lock);
          g_log_cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return g_written > pos; });
          if (g_written - pos > kRing) pos = g_written - kRing;
          chunk.clear();
          for (; pos < g_written; pos++) chunk.push_back(g_ring[pos % kRing]);
        }
        if (!chunk.empty() && !SendAll(fd, chunk.data(), chunk.size())) break;
        if (chunk.empty() && send(fd, "", 0, 0) < 0) break;
      }
      break;
    } else if (cmd == "shot") {
      std::vector<uint8_t> px;
      uint32_t w = 0, h = 0;
      if (!lb_dbg_shot(&px, &w, &h, 5000)) { if (!Reply(fd, false, "no frame within 5 s")) break; continue; }
      char head[64];
      int n = std::snprintf(head, sizeof(head), "RGBA %u %u\n", w, h);
      std::string body(head, size_t(n));
      body.append(reinterpret_cast<const char*>(px.data()), px.size());
      if (!Reply(fd, true, body)) break;
    } else if (cmd == "skip" || cmd == "unskip") {
      // skip <PS hash hex>: draws with that pixel shader are skipped; unskip [hash] (no hash: all).
      const uint64_t h = a.size() > 1 ? std::strtoull(a[1].c_str(), nullptr, 16) : 0;
      lb_dbg_skip(h, cmd == "skip");
      if (!Reply(fd, true, "")) break;
    } else if (cmd == "flag" && a.size() >= 3) {
      lb_dbg_flag(std::atoi(a[1].c_str()), std::atoi(a[2].c_str()));
      if (!Reply(fd, true, "")) break;
    } else if (cmd == "mem" && a.size() >= 3) {
      // mem <physical address hex> <bytes>: raw guest memory (at most 4 MB).
      std::string body;
      if (!lb_dbg_mem(uint32_t(std::strtoul(a[1].c_str(), nullptr, 16)), uint32_t(std::strtoul(a[2].c_str(), nullptr, 0)), &body)) {
        if (!Reply(fd, false, "bad range")) break;
        continue;
      }
      if (!Reply(fd, true, body)) break;
    } else if (cmd == "draws") {
      std::vector<std::string> list;
      if (!lb_dbg_draws(&list, 5000)) { if (!Reply(fd, false, "no frame within 5 s")) break; continue; }
      std::string body;
      for (auto& l : list) { body += l; body += '\n'; }
      if (!Reply(fd, true, body)) break;
    } else if (cmd == "press" || cmd == "hold" || cmd == "release") {
      bool ok = true;
      uint16_t bits = a.size() > 1 ? Buttons(a[1], &ok) : 0xFFFF;
      if (!ok) { if (!Reply(fd, false, "unknown button")) break; continue; }
      lb_dbg_input_on = 1;
      if (cmd == "release") {
        lb_dbg_buttons = uint16_t(lb_dbg_buttons & ~bits);
      } else {
        lb_dbg_buttons = uint16_t(lb_dbg_buttons | bits);
        if (cmd == "press") {
          const int ms = a.size() > 2 ? std::atoi(a[2].c_str()) : 120;
          svcSleepThread(int64_t(ms) * 1000000);
          lb_dbg_buttons = uint16_t(lb_dbg_buttons & ~bits);
        }
      }
      if (!Reply(fd, true, "")) break;
    } else if (cmd == "stick" && a.size() >= 4) {
      const int16_t x = int16_t(std::max(-1.0, std::min(1.0, std::atof(a[2].c_str()))) * 32767);
      const int16_t y = int16_t(std::max(-1.0, std::min(1.0, std::atof(a[3].c_str()))) * 32767);
      lb_dbg_input_on = 1;
      if (a[1] == "R" || a[1] == "r") { lb_dbg_rx = x; lb_dbg_ry = y; } else { lb_dbg_lx = x; lb_dbg_ly = y; }
      if (!Reply(fd, true, "")) break;
    } else if (cmd == "trigger" && a.size() >= 3) {
      const uint8_t v = uint8_t(std::max(0.0, std::min(1.0, std::atof(a[2].c_str()))) * 255);
      lb_dbg_input_on = 1;
      if (a[1] == "R" || a[1] == "r") lb_dbg_rt = v; else lb_dbg_lt = v;
      if (!Reply(fd, true, "")) break;
    } else if (cmd == "get" && a.size() >= 2) {
      FILE* f = std::fopen(PathOf(a[1]).c_str(), "rb");
      if (!f) { if (!Reply(fd, false, "cannot open")) break; continue; }
      std::string body;
      char buf[65536];
      size_t k;
      while ((k = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, k);
      std::fclose(f);
      if (!Reply(fd, true, body)) break;
    } else if (cmd == "put" && a.size() >= 3) {
      const size_t size = size_t(std::strtoull(a[2].c_str(), nullptr, 10));
      const std::string path = PathOf(a[1]), part = path + ".part";
      FILE* f = std::fopen(part.c_str(), "wb");
      std::vector<char> buf(65536);
      size_t left = size;
      bool ok = f != nullptr;
      while (left) {
        size_t k = std::min(left, buf.size());
        if (!RecvAll(fd, buf.data(), k)) { ok = false; left = 0; break; }
        if (f && std::fwrite(buf.data(), 1, k, f) != k) ok = false;
        left -= k;
      }
      if (f) std::fclose(f);
      if (ok) { std::remove(path.c_str()); ok = std::rename(part.c_str(), path.c_str()) == 0; }
      char b[64];
      std::snprintf(b, sizeof(b), "size %zu", size);
      if (!Reply(fd, ok, ok ? b : "write failed")) break;
    } else if (cmd == "ls" && a.size() >= 2) {
      DIR* d = opendir(PathOf(a[1]).c_str());
      if (!d) { if (!Reply(fd, false, "cannot open")) break; continue; }
      std::string body;
      while (dirent* e = readdir(d)) {
        struct stat st {};
        std::string p = PathOf(a[1]);
        if (p.back() != '/') p += '/';
        stat((p + e->d_name).c_str(), &st);
        char b[512];
        std::snprintf(b, sizeof(b), "%s%s %lld\n", e->d_name, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size);
        body += b;
      }
      closedir(d);
      if (!Reply(fd, true, body)) break;
    } else if (cmd == "rm" && a.size() >= 2) {
      if (!Reply(fd, std::remove(PathOf(a[1]).c_str()) == 0, "")) break;
    } else {
      if (!Reply(fd, false, "unknown command")) break;
    }
  }
  close(fd);
}

void Listener(int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) { std::printf("[dbg] socket failed\n"); return; }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(uint16_t(port));
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(s, 4) < 0) {
    std::printf("[dbg] bind/listen on %d failed\n", port);
    close(s);
    return;
  }
  const u32 ip = gethostid();
  std::printf("[dbg] debug server listening on %u.%u.%u.%u:%d\n", ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24, port);
  while (true) {
    int c = accept(s, nullptr, nullptr);
    if (c < 0) { svcSleepThread(100000000); continue; }
    // One connection at a time on this thread. A thread per connection (never joined: libnx has no
    // pthread_detach) leaked one thread per command; after a few hundred commands the next std::thread threw
    // std::system_error and std::terminate closed the game (console, 2026-11-10). The tools send one command at a time.
    try {
      Client(c);
    } catch (...) {
      std::printf("[dbg] a client failed; the server keeps listening\n");
    }
  }
}

}  // namespace

// Call after stdout was freopen'ed to its log file.
extern "C" void lb_dbg_start(const char* root, int port) {
  if (root && *root) { g_root = root; if (g_root.back() != '/') g_root += '/'; }
  std::fflush(stdout);
  g_orig_write = stdout->_write;
  stdout->_write = TeeWrite;
  if (R_FAILED(socketInitializeDefault())) { std::printf("[dbg] socketInitialize failed\n"); return; }
  new std::thread(Listener, port);  // see Client: no detach on libnx
}
