// The shader hash shared by the trace and deko3d backends: 64-bit FNV-1a over the ucode bytes.
// The trace backend's --dump_shaders files, the GLSL/DKSH files (tools/xenos2glsl.py) and the
// deko3d backend's lookups are all named by it. cvhd-recomp, BSD-3 like the SDK.
#pragma once
#include <cstddef>
#include <cstdint>

namespace rex::graphics::cvhd {

inline uint64_t UcodeHash(const uint32_t* dwords, uint32_t count) {
  uint64_t h = 1469598103934665603ull;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(dwords);
  for (size_t i = 0; i < size_t(count) * 4; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace rex::graphics::cvhd
