// Finds Xbox 360 (Xenos) shader microcode in a file by decoding its control-flow program, without the TT package
// format: at each 4-byte position, the control-flow instructions (two per 3 big-endian dwords) must fill exactly the
// instruction slots before the first exec, use valid opcodes, include an "end" exec and an alloc, and every exec must
// address slots after the control flow. The code length is max(exec address + count) slots of 12 bytes.
// Output, one line per shader: <offset hex> <length bytes> <V|P> (V when it allocs a vertex position).
//   ucode_scan <file> [align]
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// d0: 32-bit word, d1: 16-bit word of one control-flow instruction.
static int cf_check(uint32_t d0, uint32_t d1, uint32_t* min_exec, uint32_t* max_end, int* has_end, int* alloc_mask) {
  const uint32_t op = (d1 >> 12) & 0xF;
  switch (op) {
    case 0: return 1;  // nop
    case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14: {  // exec family
      const uint32_t addr = d0 & 0xFFF, cnt = (d0 >> 12) & 7;
      if (cnt == 0) return op == 2 || op == 4 || op == 6 || op == 14 ? (*has_end = 1, 1) : 1;  // empty exec(end) is legal
      if (addr < *min_exec) *min_exec = addr;
      if (addr + cnt > *max_end) *max_end = addr + cnt;
      if (op == 2 || op == 4 || op == 6 || op == 14) *has_end = 1;
      return 1;
    }
    case 7: case 8: case 9: case 10: case 11: case 15: return 1;  // loops, calls, jumps, vfetch done
    case 12: *alloc_mask |= 1 << ((d1 >> 9) & 3); return 1;        // alloc
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const size_t align = argc > 2 ? strtoul(argv[2], 0, 0) : 4;
  int fd = open(argv[1], O_RDONLY);
  struct stat st;
  fstat(fd, &st);
  const uint8_t* m = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  const size_t n = st.st_size;
  for (size_t p = 0; p + 12 <= n; p += align) {
    uint32_t min_exec = 4096, max_end = 0, slot = 0;
    int has_end = 0, alloc_mask = 0, ok = 1;
    // A shader's control flow fills slots 0 .. first exec address - 1 (at most 32 here: real shaders use few).
    for (; slot < min_exec && slot < 32; slot++) {
      const size_t q = p + slot * 12;
      if (q + 12 > n) { ok = 0; break; }
      const uint32_t w0 = be32(m + q), w1 = be32(m + q + 4), w2 = be32(m + q + 8);
      if (!cf_check(w0, w1 & 0xFFFF, &min_exec, &max_end, &has_end, &alloc_mask)) { ok = 0; break; }
      if (!cf_check((w1 >> 16) | (w2 << 16), w2 >> 16, &min_exec, &max_end, &has_end, &alloc_mask)) { ok = 0; break; }
    }
    if (!ok || !has_end || !alloc_mask || min_exec != slot || max_end <= min_exec || max_end > 1024) continue;
    const size_t len = (size_t)max_end * 12;
    if (p + len > n) continue;
    printf("%zx %zu %c\n", p, len, (alloc_mask & 2) ? 'V' : 'P');
  }
  return 0;
}
