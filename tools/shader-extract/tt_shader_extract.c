// Extracts every Xbox 360 (Xenos) shader microcode blob from a TT Games data file (GAME.DAT), uncompressed or inside
// LZ2K-compressed chunks, and writes each unique one as <outdir>/shader_<FNV-1a 64>.ucode.be.<vert|frag> (big-endian,
// as the game hands it to the GPU; the name is the hash the deko3d backend looks shaders up by, src/gpu/ucode_hash.h).
//
// LZ2K: decoder written from the public-domain specification at https://github.com/LunaticInAHat/liblz2k (README):
// MSB-first bitstream; three canonical prefix decoders (code lengths: 19 symbols, literal/length: 510, offset: 14);
// LZSS with an 8 KiB window; blocks of BIB symbols, each with its own code tables. Chunks are "LZ2K", uncompressed size
// (LE32), compressed size (LE32), data. Consecutive chunks are one file and are decompressed into one buffer, so a
// shader that crosses a 16 KiB chunk boundary is still found.
//
// Microcode is found by decoding its control flow (see ucode_scan.c): instructions in pairs per 3 dwords filling the
// slots before the first exec, valid opcodes, an end exec and an alloc. The stored length is one slot (12 bytes) more
// than the last exec reaches (checked against 43 shaders found uncompressed on the disc).
//
//   tt_shader_extract <outdir> <file>...
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ----- bitstream ----------------------------------------------------------------------------------------------------
typedef struct { const uint8_t* p; size_t n, pos; uint32_t bit; } Bits;  // pos: byte, bit: 0..7 within it (MSB first)
static inline uint32_t bs_get(Bits* b, int n) {
  uint32_t v = 0;
  while (n-- > 0) {
    uint32_t bit = 0;
    if (b->pos < b->n) {
      bit = (b->p[b->pos] >> (7 - b->bit)) & 1;
      if (++b->bit == 8) { b->bit = 0; b->pos++; }
    }
    v = (v << 1) | bit;
  }
  return v;
}

// ----- canonical prefix decoder -------------------------------------------------------------------------------------
typedef struct {
  int as;            // alphabet size
  int ssym;          // single-symbol mode: the symbol, else -1
  uint8_t cl[512];   // code length per symbol (0 = absent)
  int count[17];     // symbols per length
  int first[17];     // first code at each length
  int offset[17];    // index of that length's first symbol in sorted[]
  uint16_t sorted[512];
} Coder;

static void sc_generate(Coder* c) {
  memset(c->count, 0, sizeof(c->count));
  for (int s = 0; s < c->as; s++) c->count[c->cl[s]]++;
  uint32_t next = 0;
  int idx = 0;
  for (int len = 1; len <= 16; len++) {
    c->first[len] = (int)(next >> (16 - len));
    c->offset[len] = idx;
    for (int s = 0; s < c->as; s++)
      if (c->cl[s] == len) { c->sorted[idx++] = (uint16_t)s; next += 1u << (16 - len); }
  }
}

static int sc_decode(Coder* c, Bits* b) {
  if (c->ssym >= 0) return c->ssym;
  int code = 0;
  for (int len = 1; len <= 16; len++) {
    code = (code << 1) | (int)bs_get(b, 1);
    const int d = code - c->first[len];
    if (c->count[len] && d >= 0 && d < c->count[len]) return c->sorted[c->offset[len] + d];
  }
  return -1;
}

static void sc_init_hybrid(Coder* c, Bits* b, int as, int l2as, int s1c) {
  c->as = as;
  memset(c->cl, 0, sizeof(c->cl));
  int us = (int)bs_get(b, l2as);
  if (us == 0) { c->ssym = (int)bs_get(b, l2as); if (c->ssym >= as) c->ssym = 0; return; }
  c->ssym = -1;
  int sid = 0;
  while (sid < us && sid < as) {
    int cl = (int)bs_get(b, 3);
    if (cl == 7) while (bs_get(b, 1) && cl < 16) cl++;
    c->cl[sid++] = (uint8_t)cl;
    if (sid == s1c) sid += (int)bs_get(b, 2);
  }
  sc_generate(c);
}

static void sc_init_coded(Coder* c, Bits* b, int as, int l2as, Coder* dec) {
  c->as = as;
  memset(c->cl, 0, sizeof(c->cl));
  int us = (int)bs_get(b, l2as);
  if (us == 0) { c->ssym = (int)bs_get(b, l2as); if (c->ssym >= as) c->ssym = 0; return; }
  c->ssym = -1;
  int sid = 0;
  while (sid < us && sid < as) {
    const int sym = sc_decode(dec, b);
    if (sym < 0) break;
    if (sym == 0) c->cl[sid++] = 0;
    else if (sym == 1) sid += 3 + (int)bs_get(b, 4);
    else if (sym == 2) sid += 20 + (int)bs_get(b, 9);
    else c->cl[sid++] = (uint8_t)(sym - 2);
  }
  sc_generate(c);
}

// Decompresses one LZ2K chunk (without its 12-byte header) into out[0..usize). Returns 0 on success.
static int lz2k_decompress(const uint8_t* in, size_t csize, uint8_t* out, size_t usize) {
  Bits b = {in, csize, 0, 0};
  static Coder cl, lit, off;
  uint32_t bib = 0;
  size_t o = 0;
  while (o < usize) {
    if (bib == 0) {
      bib = bs_get(&b, 16);
      sc_init_hybrid(&cl, &b, 19, 5, 3);
      sc_init_coded(&lit, &b, 510, 9, &cl);
      sc_init_hybrid(&off, &b, 14, 4, -1);
      if (bib == 0) return 1;
    }
    bib--;
    const int sym = sc_decode(&lit, &b);
    if (sym < 0) return 2;
    if (sym <= 255) { out[o++] = (uint8_t)sym; continue; }
    const int rl = sym - 253;
    const int osym = sc_decode(&off, &b);
    if (osym < 0) return 3;
    const size_t dist = osym == 0 ? 1 : 1 + (1u << (osym - 1)) + bs_get(&b, osym - 1);
    if (dist > o) return 4;
    for (int i = 0; i < rl && o < usize; i++, o++) out[o] = out[o - dist];
  }
  return 0;
}

// ----- microcode scan -----------------------------------------------------------------------------------------------
static inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static int cf_check(uint32_t d0, uint32_t d1, uint32_t* min_exec, uint32_t* max_end, int* has_end, int* alloc_mask) {
  const uint32_t op = (d1 >> 12) & 0xF;
  switch (op) {
    case 0: return 1;
    case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14: {
      const uint32_t addr = d0 & 0xFFF, cnt = (d0 >> 12) & 7;
      const int end = op == 2 || op == 4 || op == 6 || op == 14;
      if (end) *has_end = 1;
      if (cnt == 0) return 1;
      if (addr < *min_exec) *min_exec = addr;
      if (addr + cnt > *max_end) *max_end = addr + cnt;
      return 1;
    }
    case 7: case 8: case 9: case 10: case 11: case 15: return 1;
    case 12: *alloc_mask |= 1 << ((d1 >> 9) & 3); return 1;
  }
  return 0;
}

// Hash set of shaders already written.
static uint64_t* g_seen;
static size_t g_seen_cap = 1 << 20, g_seen_n;
static int seen_insert(uint64_t h) {
  size_t i = (size_t)(h * 0x9E3779B97F4A7C15ull) & (g_seen_cap - 1);
  while (g_seen[i]) {
    if (g_seen[i] == h) return 0;
    i = (i + 1) & (g_seen_cap - 1);
  }
  g_seen[i] = h;
  g_seen_n++;
  return 1;
}

static const char* g_outdir;
static size_t g_hits, g_written, g_written_v;

// Real shaders only. TT stores a file's shader container headers (D3D 2008 format: 0x102A1100 pixel / 0x102A1101
// vertex) in one section and their microcode in another, in the same order. So in each buffer (one TT file,
// decompressed) the container headers are listed in order, the microcode hits too, and they are paired in order: header
// k takes the first hit after header k-1's hit with the same code size and stage. Random data that happens to decode as
// control flow (thousands of hits) is left unpaired.
typedef struct { uint32_t size; int vert; } Cont;
typedef struct { size_t off; uint32_t len; int vert; } Hit;
static Cont* g_conts;
static Hit* g_hits_buf;
static size_t g_conts_cap = 1 << 16, g_hits_cap = 1 << 20;

static void scan(const uint8_t* m, size_t n) {
  size_t nc = 0, nh = 0;
  for (size_t i = 0; i + 36 <= n; i += 4) {
    if (m[i] != 0x10 || m[i + 1] != 0x2A || m[i + 2] != 0x11 || m[i + 3] > 1) continue;
    const uint32_t vs = be32(m + i + 4), ps = be32(m + i + 8), so = be32(m + i + 24);
    if (be32(m + i + 28) || be32(m + i + 32) || vs > 0x100000 || ps > 0x100000 || so + 8 > vs || i + so + 8 > n) continue;
    const uint32_t size = be32(m + i + so + 4);
    if (size == 0 || size > 0x10000 || nc == g_conts_cap) continue;
    g_conts[nc++] = (Cont){size, m[i + 3] & 1};
  }
  if (!nc) return;
  for (size_t p = 0; p + 12 <= n && nh < g_hits_cap; p += 4) {
    uint32_t min_exec = 4096, max_end = 0, slot = 0;
    int has_end = 0, alloc_mask = 0, ok = 1;
    for (; slot < min_exec && slot < 32; slot++) {
      const size_t q = p + slot * 12;
      if (q + 12 > n) { ok = 0; break; }
      const uint32_t w0 = be32(m + q), w1 = be32(m + q + 4), w2 = be32(m + q + 8);
      if (!cf_check(w0, w1 & 0xFFFF, &min_exec, &max_end, &has_end, &alloc_mask)) { ok = 0; break; }
      if (!cf_check((w1 >> 16) | (w2 << 16), w2 >> 16, &min_exec, &max_end, &has_end, &alloc_mask)) { ok = 0; break; }
    }
    if (!ok || !has_end || !alloc_mask || min_exec != slot || max_end <= min_exec || max_end > 1024) continue;
    const size_t len = ((size_t)max_end + 1) * 12;
    if (p + len > n) continue;
    g_hits_buf[nh++] = (Hit){p, (uint32_t)len, (alloc_mask & 2) != 0};
  }
  size_t h = 0;
  for (size_t c = 0; c < nc && h < nh; c++) {
    size_t k = h;
    while (k < nh && !(g_hits_buf[k].len == g_conts[c].size && g_hits_buf[k].vert == g_conts[c].vert)) k++;
    if (k == nh) continue;  // this header's code is not in this buffer (or not recognised): next header
    const Hit* hit = &g_hits_buf[k];
    h = k + 1;
    g_hits++;
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < hit->len; i++) hash = (hash ^ m[hit->off + i]) * 1099511628211ull;
    if (!seen_insert(hash)) continue;
    char path[512];
    snprintf(path, sizeof(path), "%s/shader_%016llX.ucode.be.%s", g_outdir, (unsigned long long)hash, hit->vert ? "vert" : "frag");
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(m + hit->off, 1, hit->len, f); fclose(f); g_written++; g_written_v += hit->vert; }
  }
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <outdir> <file>...\n", argv[0]); return 2; }
  g_outdir = argv[1];
  g_seen = calloc(g_seen_cap, sizeof(uint64_t));
  g_conts = malloc(g_conts_cap * sizeof(Cont));
  g_hits_buf = malloc(g_hits_cap * sizeof(Hit));
  size_t run_cap = 64u << 20;
  uint8_t* run = malloc(run_cap);
  for (int a = 2; a < argc; a++) {
    int fd = open(argv[a], O_RDONLY);
    if (fd < 0) { perror(argv[a]); continue; }
    struct stat st;
    fstat(fd, &st);
    const size_t n = st.st_size;
    const uint8_t* m = mmap(0, n, PROT_READ, MAP_PRIVATE, fd, 0);
    size_t chunks = 0, bad = 0, runs = 0;
    scan(m, n);  // uncompressed shaders
    size_t p = 0;
    while (p + 12 <= n) {
      const uint8_t* t = memmem(m + p, n - p, "LZ2K", 4);
      if (!t) break;
      p = (size_t)(t - m);
      // A run of back-to-back chunks: one file.
      size_t used = 0;
      while (p + 12 <= n && memcmp(m + p, "LZ2K", 4) == 0) {
        const uint32_t usize = m[p + 4] | m[p + 5] << 8 | m[p + 6] << 16 | (uint32_t)m[p + 7] << 24;
        const uint32_t csize = m[p + 8] | m[p + 9] << 8 | m[p + 10] << 16 | (uint32_t)m[p + 11] << 24;
        if (usize == 0 || usize > (1u << 20) || csize == 0 || p + 12 + csize > n) { p += 4; break; }
        if (used + usize > run_cap) { scan(run, used); used = 0; }
        if (lz2k_decompress(m + p + 12, csize, run + used, usize) != 0) { bad++; p += 12 + csize; break; }
        used += usize;
        chunks++;
        p += 12 + csize;
      }
      if (used) { scan(run, used); runs++; }
    }
    fprintf(stderr, "%s: %zu LZ2K chunks in %zu runs (%zu failed); %zu shader hits, %zu unique written (%zu vertex)\n",
            argv[a], chunks, runs, bad, g_hits, g_written, g_written_v);
    munmap((void*)m, n);
    close(fd);
  }
  return 0;
}
