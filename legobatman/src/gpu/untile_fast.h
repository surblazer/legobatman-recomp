// Fast untiling of Xenos 2D textures (the layout of Xenia's texture_conversion::Untile, which calls a
// std::function per block): the same offsets computed inline, the blocks swapped by their endian mode in
// place. Block sizes 1, 2, 4, 8 and 16 bytes (every format the deko3d backend uploads). The
// first stage's backdrop textures are 4320x2432 RGBA8 (10.5 M blocks): the callback version costs about a
// second each on a PC. Tested against Untile by tests/untile_test.cpp. BSD-3 like the SDK.
#pragma once
#include <cstdint>
#include <cstring>

namespace cvhd {

enum class SwapMode : uint8_t { kNone = 0, k8in16 = 1, k8in32 = 2, k16in32 = 3 };

namespace untile_detail {

inline uint32_t Swap32(uint32_t v, SwapMode m) {
  switch (m) {
    case SwapMode::k8in32: return __builtin_bswap32(v);
    case SwapMode::k8in16: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case SwapMode::k16in32: return (v << 16) | (v >> 16);
    default: return v;
  }
}

// One block. The guest's endian swap works on the memory dwords: a block of 4 bytes or more is swapped by its
// own dwords; a smaller block (k_8, k_8_8) is cut out of its dword after the dword was swapped (k8in32 also
// reverses the order of the blocks inside the dword).
template <uint32_t kLog2>
inline void CopyBlock(uint8_t* dst, const uint8_t* src, SwapMode m) {
  if constexpr (kLog2 >= 2) {
    constexpr uint32_t words = (1u << kLog2) / 4;
    for (uint32_t i = 0; i < words; i++) {
      uint32_t v;
      std::memcpy(&v, src + i * 4, 4);
      v = Swap32(v, m);
      std::memcpy(dst + i * 4, &v, 4);
    }
  } else {
    if (m == SwapMode::kNone) {
      std::memcpy(dst, src, 1u << kLog2);
      return;
    }
    const uint8_t* base = reinterpret_cast<const uint8_t*>(uintptr_t(src) & ~uintptr_t(3));
    uint32_t within = uint32_t(uintptr_t(src) & 3);
    uint32_t v;
    std::memcpy(&v, base, 4);
    v = Swap32(v, m);
    uint8_t tmp[4];
    std::memcpy(tmp, &v, 4);
    std::memcpy(dst, tmp + within, 1u << kLog2);
  }
}

template <uint32_t kLog2>
void Untile(uint8_t* dst, const uint8_t* src, uint32_t width_blocks, uint32_t height_blocks, uint32_t pitch_blocks,
            SwapMode swap, uint32_t origin_x, uint32_t origin_y) {
  constexpr uint32_t bpb = 1u << kLog2;
  for (uint32_t oy = 0; oy < height_blocks; oy++) {
    const uint32_t y = oy + origin_y;
    // TiledOffset2DRow
    uint32_t macro_row = ((y / 32) * (pitch_blocks / 32)) << (kLog2 + 7);
    uint32_t micro_row = ((y & 6) << 2) << kLog2;
    uint32_t row = macro_row + ((micro_row & ~0xFu) << 1) + (micro_row & 0xFu) + ((y & 8) << (3 + kLog2)) + ((y & 1) << 4);
    uint32_t y16 = (y & 16) << 7;
    uint32_t y8 = (y & 8) >> 2;
    uint8_t* out = dst + size_t(oy) * width_blocks * bpb;
    for (uint32_t ox = 0; ox < width_blocks; ox++) {
      const uint32_t x = ox + origin_x;
      // TiledOffset2DColumn
      uint32_t macro = (x / 32) << (kLog2 + 7);
      uint32_t micro = (x & 7) << kLog2;
      uint32_t offset = row + (macro + ((micro & ~0xFu) << 1) + (micro & 0xFu));
      uint32_t o = ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0u) << 2) + (offset & 0x3Fu) + y16 + (((y8 + (x >> 3)) & 3) << 6);
      CopyBlock<kLog2>(out + size_t(ox) * bpb, src + size_t(o >> kLog2) * bpb, swap);
    }
  }
}

}  // namespace untile_detail

// dst: width_blocks x height_blocks tightly packed; src: the tiled texture, `pitch_blocks` (a multiple of 32)
// blocks per row. origin_x/y: where the image starts in the tiled surface, in blocks (a base level stored in a packed
// mip tail: a 16x24 texture sits at x 16 of its 32x32 tile). Returns false for an unsupported block size.
inline bool UntileFast(uint8_t* dst, const uint8_t* src, uint32_t width_blocks, uint32_t height_blocks,
                       uint32_t pitch_blocks, uint32_t bytes_per_block, SwapMode swap, uint32_t origin_x = 0,
                       uint32_t origin_y = 0) {
  switch (bytes_per_block) {
    case 1: untile_detail::Untile<0>(dst, src, width_blocks, height_blocks, pitch_blocks, swap, origin_x, origin_y); return true;
    case 2: untile_detail::Untile<1>(dst, src, width_blocks, height_blocks, pitch_blocks, swap, origin_x, origin_y); return true;
    case 4: untile_detail::Untile<2>(dst, src, width_blocks, height_blocks, pitch_blocks, swap, origin_x, origin_y); return true;
    case 8: untile_detail::Untile<3>(dst, src, width_blocks, height_blocks, pitch_blocks, swap, origin_x, origin_y); return true;
    case 16: untile_detail::Untile<4>(dst, src, width_blocks, height_blocks, pitch_blocks, swap, origin_x, origin_y); return true;
    default: return false;
  }
}

}  // namespace cvhd
