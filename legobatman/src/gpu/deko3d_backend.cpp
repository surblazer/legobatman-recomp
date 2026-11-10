// deko3d GPU backend (--gpu_backend=deko3d, Nintendo Switch): the SDK's Xenos command processor
// parses the ring (fences, read-pointer write-back, vblank, bin masks), this backend draws with
// deko3d. No EDRAM model: a host image per render target (keyed by EDRAM base and pitch), resolves
// are image copies into images keyed by guest address, textures bound at such an address alias
// them (no readback), the swap presents the image at the front buffer's address through one
// textured triangle. Shaders are translated offline (tools/xenos2glsl.py, uam) and loaded by the
// ucode hash from sdmc:/switch/cvhd/shaders/. docs/native-renderer-plan.md, M3.
// cvhd-recomp, BSD-3 like the SDK.
// Linux (CVHD_DK_GL): the same code over an OpenGL stand-in for libdeko3d (dk_gl/dk_gl_shim.cpp), a test
// host for the backend logic, with the shaders as translated GLSL instead of DKSH.
#if defined(__SWITCH__) || (defined(__linux__) && __has_include(<EGL/egl.h>))
#ifdef __SWITCH__
#include <switch.h>
#include <deko3d.h>
#else
#define CVHD_DK_GL 1
#include "dk_gl/switch.h"
#include "dk_gl/dk_gl_shim.h"
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <thread>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/string/buffer.h>
#include <rex/system/gpu_plugin.h>

// LEGO Batman port: texture change detection by a SAMPLED hash of the guest memory (32 bytes of every 4 KB page),
// at most once per image per frame. History: a full hash at every bind cost ~75% of the frame; the SDK's page
// watches (unmapping pages on the Switch) fragmented the address space until Horizon ran out of memory blocks
// (svcMapProcessMemory 0xCE01) and a page that could not be mapped back crashed the game (2026-11-09).
#define LEGOBATMAN_HASH_TEXTURES 1

#include "ucode_hash.h"
#include "untile_fast.h"
#ifndef CVHD_DK_GL
#include <atomic>
#include <condition_variable>
#include <mutex>
// LEGO Batman port: the debug server's "shot" (legobatman/switch/host/debug_server.cpp): the next presented frame.
namespace lbshot {
std::mutex lock;
std::condition_variable cv;
bool requested = false, ready = false;
std::vector<uint8_t> px;
uint32_t w = 0, h = 0;
// "skip <ps hash>" and "draws" (the next frame's draw list) of the debug server.
std::atomic<uint64_t> skip_ps[8];
std::atomic<int> flags[32];  // "flag N V": 0 = re-upload linear/dynamic textures at every frame's first bind
std::atomic<int> want_draws{0};  // 1 = collect the next frame, 2 = collecting
std::vector<std::string> draws;
bool draws_ready = false;
}  // namespace lbshot
void lb_dbg_skip(uint64_t ps, bool on) {
  for (auto& slot : lbshot::skip_ps) {
    uint64_t cur = slot.load();
    if (on && cur == 0) { slot = ps; return; }
    if (!on && (cur == ps || ps == 0)) slot = 0;
  }
}
void lb_dbg_flag(int n, int v) { if (n >= 0 && n < 32) lbshot::flags[n] = v; }
bool lb_dbg_draws(std::vector<std::string>* out, int timeout_ms) {
  std::unique_lock<std::mutex> lk(lbshot::lock);
  lbshot::draws.clear();
  lbshot::draws_ready = false;
  lbshot::want_draws = 1;
  if (!lbshot::cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return lbshot::draws_ready; })) return false;
  *out = std::move(lbshot::draws);
  return true;
}
bool lb_dbg_shot(std::vector<uint8_t>* rgba, uint32_t* w, uint32_t* h, int timeout_ms) {
  std::unique_lock<std::mutex> lk(lbshot::lock);
  lbshot::ready = false;
  lbshot::requested = true;
  if (!lbshot::cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return lbshot::ready; })) {
    lbshot::requested = false;
    return false;
  }
  *rgba = std::move(lbshot::px);
  *w = lbshot::w;
  *h = lbshot::h;
  return true;
}
#endif

extern "C" volatile unsigned rex_nx_swaps;     // switch_nx.cpp: the busy/frame line divides by it
extern "C" volatile double rex_nx_main_busy_ms;  // switch_nx.cpp: the main thread's busy ms per frame
extern "C" void rex_nx_waited_add(unsigned long long ticks);  // switch_nx.cpp: the busy/frame line
#include "../../switch/host/include/rex/nx_menu.h"  // switch/host/nx_menu.cpp: the settings and the overlay the present pass draws
#ifndef CVHD_DK_GL
#include "../../switch/host/include/rex/frame_recorder.h"  // switch/host/frame_recorder.cpp: RECORD 5 SEC
#endif
extern "C" volatile int rex_nx_vblank_divider;  // SDK graphics_system.cpp (sdk-edits rule 31): vblanks per guest vblank interrupt
#ifdef CVHD_DK_GL
volatile int rex_nx_vblank_divider = 1;
volatile unsigned rex_nx_swaps = 0;  // no switch_nx.cpp on the Linux test host
volatile double rex_nx_main_busy_ms = 0;
extern "C" void rex_nx_waited_add(unsigned long long) {}
extern "C" int rex_nx_setting_fps_mode(void) { return 0; }  // no menu on the Linux test host
extern "C" int rex_nx_setting_fps_counter(void) { return 1; }
extern "C" int rex_nx_debug_record(void) { return 0; }
extern "C" void rex_nx_debug_record_set(int) {}
extern "C" int rex_nx_menu_overlay(NxOverlay* out, const char*, int, int) { std::memset(out, 0, sizeof(*out)); return 0; }
#endif

// Where the DKSH shaders are (tools/xenos2glsl.py + uam, staged by switch/build.sh): a game sets it
// in its cvhd.toml / arguments. Missing shaders are dumped to its missing/ subdirectory. The debug
// markers are files next to the directory: dk_dump (image readback at swaps 120 and 600),
// dk_present_rt (present the widest colour render target instead of the front buffer).
REXCVAR_DEFINE_STRING(deko3d_shader_dir, "sdmc:/switch/legobatman/shaders/"  /* LEGO Batman port */, "GPU",
                      "deko3d backend: directory of shader_<ucode hash>.vert|geom|frag.dksh and present.vert|frag.dksh")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Xenos rectangle lists (3 vertices, the 4th = v1 + v2 - v0): by default through the geometry shader
// the translator writes next to each vertex shader; the hardware fill-rectangle mode (NV_fill_rectangle,
// the same semantics) is an option for the console: Eden ignores that mode (one triangle is drawn).
REXCVAR_DEFINE_BOOL(deko3d_fill_rectangle, false, "GPU",
                    "deko3d backend: draw rectangle lists with the hardware fill-rectangle mode instead of a geometry shader")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(deko3d_cull, 1, "GPU",
                     "deko3d backend: 0 = no face culling, 1 = cull as the guest asks, 2 = the same with the winding inverted")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The Switch host layer's dirty-page bytes (switch/host/memory_nx.cpp): one byte per 4 KB page of the
// physical arena, set by every guest store (the codegen pch) and by host writes into guest memory
// (file reads). This backend is their only consumer (there is no SharedMemory here).
extern "C" volatile uint8_t rex_nx_dirty[0x20000];
extern "C" volatile uint8_t rex_nx_dirty_coarse[0x200];
#ifdef CVHD_DK_GL
// No store macros mark pages on Linux: the arrays stay empty and changes are found by hashing the guest
// memory of each texture at its bind (MemoryChanged).
extern "C" volatile uint8_t rex_nx_dirty[0x20000] = {};
extern "C" volatile uint8_t rex_nx_dirty_coarse[0x200] = {};
#endif

namespace rex::graphics::deko {

namespace {

constexpr uint32_t kFrames = 2;
// LEGO Batman port: 40 MB (was 24): gameplay frames used 23.7 MB (~2,400 draws, constants change per draw).
constexpr uint32_t kFrameDataBytes = 40u << 20;  // vertex, index and uniform data of one frame
constexpr uint32_t kFrameCmdBytes = 4u << 20;
// One frame's texture uploads. The first stage's backdrop layers are 4320x2432 RGBA8 textures (41 MB each): a
// texture larger than this is not uploaded at all ("staging ring full") and its draws sample nothing. 96 MB holds
// both layers in the stage's first frame (72 MB held one; the second came a frame later, tiles missing meanwhile).
constexpr uint32_t kStagingBytes = 96u << 20;
// LEGO Batman port: log the colour census of the front buffer, render targets and big resolve images every 300 swaps.
constexpr bool kSinglePassTiles = true;  // see tiling_seen_ in the command processor
// ponytail: a bump pool, nothing is freed (the title 132 MB, the menu 185 MB): a free list with
// eviction of textures not bound for a while is the upgrade when a stage fills it.
// LEGO Batman port: 416 MB (was 448; peak use 192 MB) pays for the larger frame data rings.
constexpr uint32_t kImagePoolBytes = 272u << 20;
// LEGO Batman port (render scale): render targets and resolve destinations live in a pool of their own, reset at
// every render scale change (a bump pool never returns memory: four scale changes filled the 416 MB pool and every
// image creation failed, a black screen at 0.1 fps).
constexpr uint32_t kScaledPoolBytes = 192u << 20;
// Render scale unit: twelfths of the guest size (12 = native 720p, 11 = 660p, 10 = 600p, 9 = 540p, 8 = 480p).
constexpr uint32_t kScaleDen = 12;
constexpr uint32_t kCodePoolBytes = 4u << 20;
constexpr uint32_t kMaxImageDescriptors = 512;
constexpr uint32_t kSamplerCount = 512;  // see SamplerIndex: min, mag, border, clamp x, clamp y
constexpr uint32_t kPresentWidth = 1280;
constexpr uint32_t kPresentHeight = 720;
constexpr uint32_t kEdramTiles = 2048;  // 10 MB of 5120-byte tiles (80 x 16 x 32 bit)
constexpr uint32_t kMaxTargetHeight = 1088;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::printf("[dk] ");
  std::vprintf(fmt, ap);
  std::printf("\n");
  va_end(ap);
  std::fflush(stdout);
}

void DebugCallback(void*, const char* context, DkResult result, const char* message) {
  Log("deko3d %s: result %d: %s", context ? context : "?", int(result), message ? message : "");
}

struct MemBlock {
  DkMemBlock block = nullptr;
  uint8_t* cpu = nullptr;
  DkGpuAddr gpu = 0;
  uint32_t size = 0;
  uint32_t used = 0;
  uint32_t peak = 0;
  bool Create(DkDevice device, uint32_t bytes, uint32_t flags) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, bytes);
    maker.flags = flags;
    block = dkMemBlockCreate(&maker);
    if (!block) return false;
    size = bytes;
    cpu = (flags & DkMemBlockFlags_CpuUncached) || (flags & DkMemBlockFlags_CpuCached)
              ? static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block))
              : nullptr;
    gpu = dkMemBlockGetGpuAddr(block);
    return true;
  }
  // Bump allocation; UINT32_MAX when full.
  uint32_t Alloc(uint32_t bytes, uint32_t align) {
    uint32_t off = (used + align - 1) & ~(align - 1);
    if (off + bytes > size) return UINT32_MAX;
    used = off + bytes;
    if (used > peak) peak = used;
    return off;
  }
  void Reset() { used = 0; }
};

struct HostImage {
  DkImage image{};
  DkImageLayout layout{};
  DkImageFormat format = DkImageFormat_RGBA8_Unorm;
  uint32_t width = 0, height = 0;
  // LEGO Batman port (render scale): the size the game knows (guest pixels) and the scale in eighths the image was
  // made at (render targets and resolve destinations follow the render scale; uploaded textures are always 8 = 1:1).
  uint32_t gw = 0, gh = 0;
  uint32_t hs = kScaleDen;
  bool scaled_pool = false;   // in scaled_images_: dropped (never recycled) and its memory reset at a scale change
  bool is_depth = false;
  uint32_t flags = 0;         // gh: the layout flags (a recycled image must match)
  bool from_resolve = false;  // host-native channels: the fetch swizzle is not applied
  uint32_t tile_resolved = 0;  // swaps_ + 1 of a frame that resolved into it in tile-sized pieces (single-pass tiles)
  uint32_t tile_rows = 0;      // ... and the rows those pieces have filled so far (complete at the image height)
  uint32_t checked_frame = ~0u;  // LEGO Batman port: swap of the last change check (hash mode)
  bool full_hash = false;  // LEGO Batman port: hash every word (linear or ever-changed images: video planes)
  bool reversed_channels = false;  // the format stores the Xenos channels in reverse order (4-4-4-4)
  uint32_t content_serial = 0;  // the draw serial when the contents were set (upload or resolve); 0 = never
  uint64_t mem_hash = 0;        // Linux (no dirty bytes): hash of the guest memory when the contents were set
  uint32_t mem_bytes = 0;       // ... over this many bytes (the same range at every later check)
  uint32_t checked_serial = 0;  // the draw serial of the last dirty-page check
  uint32_t guest_base = 0;    // textures and resolve destinations
  uint32_t edram_base = 0, edram_pitch = 0;  // render targets
};

class DkShaderObj : public Shader {
 public:
  DkShaderObj(xenos::ShaderType type, uint64_t hash, const uint32_t* dwords, uint32_t count)
      : Shader(type, hash, dwords, count) {}
  DkShader shader{};
  bool loaded = false;
  DkShader geom{};           // vertex shaders: the rectangle-list geometry shader
  bool geom_loaded = false;
  // LEGO Batman port: a vertex shader translated from its disc copy (shader_<key>.gvert.dksh, LoadGenericVs): the
  // game rewrites a vertex shader's fetches when it binds it, so the disc copy has generic inputs. Input k of the
  // generic shader takes the runtime fetch with the same destination register and write mask; gen_swz[k] is that
  // fetch's destination swizzle (SwizzleSource: 0..3 = x..w, 4 = 0, 5 = 1), uniform VFetch (binding 2).
  static constexpr uint32_t kMaxGenericInputs = 32;
  bool generic = false;
  uint32_t gen_count = 0;                  // inputs of the generic shader
  std::vector<int8_t> gen_loc;             // per runtime attribute (vertex_bindings order): its input, -1 = none
  int32_t gen_swz[kMaxGenericInputs][4]{};
  // LEGO Batman port: the romfs:/shaders_alt/ build of this shader (debug server "flag 3 1", for A/B), loaded on use.
  DkShader alt[4]{};
  int alt_state[4] = {};     // per set: 0 = not tried, 1 = loaded, -1 = missing
};

struct SysConstants {
  float ndc_scale[4];
  float ndc_offset[4];
  float alpha[4];
  uint32_t bools[8];
  uint32_t loops[32];  // gh: integer loop constants i0..i31 (count | start << 8 | step << 16)
};

struct Frame {
  DkCmdBuf cmdbuf = nullptr;
  uint32_t cmd_offset = 0;
  MemBlock data;     // vertex/index/uniform ring
  MemBlock staging;  // texture uploads
  uint32_t desc_offset = 0;  // image descriptors of this frame, inside `data`? no: a separate static block
  DkFence fence{};
  bool fenced = false;
  uint32_t next_descriptor = 0;
  std::unordered_map<uint64_t, uint32_t> descriptors;  // image (and swizzle) -> descriptor slot this frame
  // The uniform blocks bound last in this frame: reused while the registers behind them do not change
  // (BindUniforms; the copies were 70% of the command thread's draw time in Eden 0.186).
  uint32_t vs_uni = UINT32_MAX, ps_uni = UINT32_MAX, sys_uni = UINT32_MAX;
  const void* vs_uni_shader = nullptr;
  const void* ps_uni_shader = nullptr;
  const void* vfetch_shader = nullptr;  // LEGO Batman port: the generic vertex shader whose VFetch block is bound
  SysConstants last_sys{};
  // Texture bindings of this frame by fetch constant (+ shader binding): the image, its descriptor+sampler handle
  // and the memory range to re-check (console 0.200: the tex phase was 3.5 ms per frame at ~1.3 bindings per draw).
  struct TexBind {
    HostImage* tex;
    uint32_t handle, base, bytes, width, height;
    bool from_resolve;
  };
  std::unordered_map<uint64_t, TexBind> texbinds;
  // LEGO Batman port: vertex/index data already copied into this frame's ring, by (guest address, bytes, endian):
  // draws sharing one buffer copied it again each time (~160 ms/s). `check` samples the source to catch a buffer the
  // game rewrites within the frame.
  struct CopiedBlock { uint32_t off, bytes; uint64_t check; };
  std::unordered_map<uint64_t, CopiedBlock> copied;
  // GPU timestamps: pipeline top at the frame's first command, bottom before the present (16-byte reports).
  uint32_t ts_off = UINT32_MAX;
  // LEGO Batman port: debug server "flag 6 1": GPU timestamps between work categories (GpuMark), for a breakdown.
  static constexpr uint32_t kMaxMarks = 2048;
  // debug server "flag 14 1": a timestamp before every draw (and resolve/clear), credited to the pixel shader
  static constexpr uint32_t kMaxPsMarks = 4096;
  uint32_t pm_off = UINT32_MAX, pm_n = 0;
  uint64_t pm_hash[kMaxPsMarks];
  uint32_t mk_off = UINT32_MAX, mk_n = 0;
  uint8_t mk_tag[kMaxMarks];
  bool rec_pending = false;  // RECORD 5 SEC: this frame's shown image waits in rec_ for the fence
  uint32_t rec_swap = 0;
  uint64_t rec_us = 0;
};

// LEGO Batman port: Xenos numbers the functions 0..7 (never, less, equal, lequal, greater, notequal, gequal, always),
// deko3d 1..8 in the same order. Passing the Xenos value through made lequal "equal" and always "gequal": every
// depth-tested draw failed against the cleared depth (the black screen, 2026-11-09).
DkCompareOp CompareOp(xenos::CompareFunction f) { return DkCompareOp(uint32_t(f) + 1); }

DkBlendFactor BlendFactor(xenos::BlendFactor f) {
  switch (f) {
    case xenos::BlendFactor::kZero: return DkBlendFactor_Zero;
    case xenos::BlendFactor::kOne: return DkBlendFactor_One;
    case xenos::BlendFactor::kSrcColor: return DkBlendFactor_SrcColor;
    case xenos::BlendFactor::kOneMinusSrcColor: return DkBlendFactor_InvSrcColor;
    case xenos::BlendFactor::kSrcAlpha: return DkBlendFactor_SrcAlpha;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return DkBlendFactor_InvSrcAlpha;
    case xenos::BlendFactor::kDstColor: return DkBlendFactor_DstColor;
    case xenos::BlendFactor::kOneMinusDstColor: return DkBlendFactor_InvDstColor;
    case xenos::BlendFactor::kDstAlpha: return DkBlendFactor_DstAlpha;
    case xenos::BlendFactor::kOneMinusDstAlpha: return DkBlendFactor_InvDstAlpha;
    case xenos::BlendFactor::kConstantColor: return DkBlendFactor_ConstColor;
    case xenos::BlendFactor::kOneMinusConstantColor: return DkBlendFactor_InvConstColor;
    case xenos::BlendFactor::kConstantAlpha: return DkBlendFactor_ConstAlpha;
    case xenos::BlendFactor::kOneMinusConstantAlpha: return DkBlendFactor_InvConstAlpha;
    case xenos::BlendFactor::kSrcAlphaSaturate: return DkBlendFactor_SrcAlphaSaturate;
    default: return DkBlendFactor_One;
  }
}

DkBlendOp BlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd: return DkBlendOp_Add;
    case xenos::BlendOp::kSubtract: return DkBlendOp_Sub;
    case xenos::BlendOp::kMin: return DkBlendOp_Min;
    case xenos::BlendOp::kMax: return DkBlendOp_Max;
    case xenos::BlendOp::kRevSubtract: return DkBlendOp_RevSub;
    default: return DkBlendOp_Add;
  }
}

bool PrimitiveOf(xenos::PrimitiveType t, DkPrimitive& out, bool& rects) {
  rects = false;
  switch (t) {
    case xenos::PrimitiveType::kPointList: out = DkPrimitive_Points; return true;
    case xenos::PrimitiveType::kLineList: out = DkPrimitive_Lines; return true;
    case xenos::PrimitiveType::kLineStrip: out = DkPrimitive_LineStrip; return true;
    case xenos::PrimitiveType::kLineLoop: out = DkPrimitive_LineLoop; return true;
    case xenos::PrimitiveType::kTriangleList: out = DkPrimitive_Triangles; return true;
    case xenos::PrimitiveType::kTriangleFan: out = DkPrimitive_TriangleFan; return true;
    case xenos::PrimitiveType::kTriangleStrip: out = DkPrimitive_TriangleStrip; return true;
    case xenos::PrimitiveType::kRectangleList: out = DkPrimitive_Triangles; rects = true; return true;
    case xenos::PrimitiveType::kQuadList: out = DkPrimitive_Quads; return true;
    case xenos::PrimitiveType::kQuadStrip: out = DkPrimitive_QuadStrip; return true;
    case xenos::PrimitiveType::kPolygon: out = DkPrimitive_Polygon; return true;
    default: return false;
  }
}

bool VertexAttribOf(const ParsedVertexFetchInstruction::Attributes& a, DkVtxAttribSize& size, DkVtxAttribType& type) {
  using VF = xenos::VertexFormat;
  DkVtxAttribType norm = a.is_integer ? (a.is_signed ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled)
                                      : (a.is_signed ? DkVtxAttribType_Snorm : DkVtxAttribType_Unorm);
  type = norm;
  switch (a.data_format) {
    case VF::k_8_8_8_8: size = DkVtxAttribSize_4x8; return true;
    case VF::k_2_10_10_10: size = DkVtxAttribSize_10_10_10_2; return true;
    case VF::k_16_16: size = DkVtxAttribSize_2x16; return true;
    case VF::k_16_16_16_16: size = DkVtxAttribSize_4x16; return true;
    case VF::k_16_16_FLOAT: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Float; return true;
    case VF::k_16_16_16_16_FLOAT: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Float; return true;
    case VF::k_32: size = DkVtxAttribSize_1x32; type = a.is_signed ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled; return true;
    case VF::k_32_32: size = DkVtxAttribSize_2x32; type = a.is_signed ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled; return true;
    case VF::k_32_32_32_32: size = DkVtxAttribSize_4x32; type = a.is_signed ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled; return true;
    case VF::k_32_FLOAT: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Float; return true;
    case VF::k_32_32_FLOAT: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Float; return true;
    case VF::k_32_32_32_FLOAT: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Float; return true;
    case VF::k_32_32_32_32_FLOAT: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Float; return true;
    case VF::k_10_11_11: case VF::k_11_11_10: size = DkVtxAttribSize_11_11_10; type = DkVtxAttribType_Float; return true;
    default: return false;
  }
}

// Guest texture format -> deko3d format. The packed 16-bit formats need care: deko3d names its formats
// by memory bit order, most significant field first (format_traits.inc in libdeko3d): RGBA4, RGB565 and
// RGB5A1 keep R in the low bits; the Xenos packs R in the high bits (R5G6B5, A1R5G5B5 with R at 14:10) and
// that is the BGR565 / BGR5A1 pair. Xenos 4-4-4-4 is R:G:B:A from the top nibble; deko3d's only 4-bit
// format has the reverse order, so its channels are reversed in the view swizzle (`reversed`).
bool TextureFormatOf(xenos::TextureFormat f, DkImageFormat& out, bool& reversed) {
  using TF = xenos::TextureFormat;
  reversed = false;
  switch (f) {
    case TF::k_8_8_8_8: case TF::k_8_8_8_8_A: out = DkImageFormat_RGBA8_Unorm; return true;
    case TF::k_8: case TF::k_8_A: case TF::k_8_B: out = DkImageFormat_R8_Unorm; return true;
    case TF::k_8_8: out = DkImageFormat_RG8_Unorm; return true;
    case TF::k_DXT1: out = DkImageFormat_RGBA_BC1; return true;
    case TF::k_DXT2_3: out = DkImageFormat_RGBA_BC2; return true;
    case TF::k_DXT4_5: out = DkImageFormat_RGBA_BC3; return true;
    case TF::k_DXN: out = DkImageFormat_RG_BC5_Unorm; return true;
    case TF::k_DXT5A: out = DkImageFormat_R_BC4_Unorm; return true;
    case TF::k_4_4_4_4: out = DkImageFormat_RGBA4_Unorm; reversed = true; return true;
    case TF::k_5_6_5: out = DkImageFormat_BGR565_Unorm; return true;
    case TF::k_1_5_5_5: out = DkImageFormat_BGR5A1_Unorm; return true;
    case TF::k_16_16_16_16_FLOAT: out = DkImageFormat_RGBA16_Float; return true;
    case TF::k_16_16_16_16: out = DkImageFormat_RGBA16_Unorm; return true;
    case TF::k_32_FLOAT: out = DkImageFormat_R32_Float; return true;
    case TF::k_32_32_FLOAT: out = DkImageFormat_RG32_Float; return true;
    case TF::k_32_32_32_32_FLOAT: out = DkImageFormat_RGBA32_Float; return true;
    case TF::k_16_FLOAT: out = DkImageFormat_R16_Float; return true;
    case TF::k_16_16_FLOAT: out = DkImageFormat_RG16_Float; return true;
    case TF::k_16: out = DkImageFormat_R16_Unorm; return true;
    case TF::k_16_16: out = DkImageFormat_RG16_Unorm; return true;
    default: return false;
  }
}

DkImageFormat ColorTargetFormatOf(xenos::ColorRenderTargetFormat f) {
  switch (f) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return DkImageFormat_RGBA8_Unorm_sRGB;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10: return DkImageFormat_RGB10A2_Unorm;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: return DkImageFormat_RGBA16_Float;
    default: return DkImageFormat_RGBA8_Unorm;
  }
}

// Xenos clamp modes as the Vulkan backend maps them (texture_cache.cpp kAddressModeMap): the half-way
// modes have no host equivalent and use the edge modes, the mirror-clamp ones need MirrorClampToEdge.
DkWrapMode WrapModeOf(xenos::ClampMode m) {
  switch (m) {
    case xenos::ClampMode::kRepeat: return DkWrapMode_Repeat;
    case xenos::ClampMode::kMirroredRepeat: return DkWrapMode_MirroredRepeat;
    case xenos::ClampMode::kClampToEdge: return DkWrapMode_ClampToEdge;
    case xenos::ClampMode::kMirrorClampToEdge: return DkWrapMode_MirrorClampToEdge;
    case xenos::ClampMode::kClampToHalfway: return DkWrapMode_ClampToEdge;
    case xenos::ClampMode::kMirrorClampToHalfway: return DkWrapMode_MirrorClampToEdge;
    case xenos::ClampMode::kClampToBorder: return DkWrapMode_ClampToBorder;
    default: return DkWrapMode_MirrorClampToEdge;
  }
}

// Sampler slot of a fetch: filters as the Vulkan backend resolves them (GetSamplerParameters: the fetch
// instruction's filter unless it says "use the fetch constant"), the clamp modes for the texture's
// dimension, the border colour only where a clamp mode uses it.
uint32_t SamplerIndex(const xenos::xe_gpu_texture_fetch_t& fetch, const ParsedTextureFetchInstruction& instr) {
  xenos::TextureFilter mag = instr.attributes.mag_filter == xenos::TextureFilter::kUseFetchConst ? fetch.mag_filter : instr.attributes.mag_filter;
  xenos::TextureFilter min = instr.attributes.min_filter == xenos::TextureFilter::kUseFetchConst ? fetch.min_filter : instr.attributes.min_filter;
  xenos::ClampMode cx, cy, cz;
  texture_util::GetClampModesForDimension(fetch, cx, cy, cz);
  bool border_white = (xenos::ClampModeUsesBorder(cx) || xenos::ClampModeUsesBorder(cy)) && fetch.border_color == xenos::BorderColor::k_ABGR_White;
  return (min == xenos::TextureFilter::kLinear ? 1u : 0u) | (mag == xenos::TextureFilter::kLinear ? 2u : 0u) | (border_white ? 4u : 0u) |
         (uint32_t(cx) << 3) | (uint32_t(cy) << 6);
}

DkImageSwizzle SwizzleOf(uint32_t s) {
  switch (s & 7) {
    case 0: return DkImageSwizzle_Red;
    case 1: return DkImageSwizzle_Green;
    case 2: return DkImageSwizzle_Blue;
    case 3: return DkImageSwizzle_Alpha;
    case 4: return DkImageSwizzle_Zero;
    default: return DkImageSwizzle_One;
  }
}

#ifndef CVHD_DK_GL
extern "C" void RexSwitchPerfCount(unsigned id);                        // SDK switch_perf.cpp: counter 0 = presents
extern "C" void RexSwitchPerfResolution(unsigned width, unsigned height);  // ... the RES the console overlay shows
#endif

class Deko3dCommandProcessor : public CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;

 protected:
  // LEGO Batman port: texture change tracking through the SDK's physical-memory watches (the mechanism its
  // SharedMemory uses) instead of cvhd's store-macro dirty bytes, which our SDK's generated code does not have.
  // A guest write (or a file read) into a watched page calls this; it marks the pages in rex_nx_dirty, which
  // SyncDirtyPages turns into page serials as on cvhd's Switch build. The range is widened to 256 KB (64 pages)
  // like the SDK: re-uploading a little extra is far cheaper than another fault.
  void* watch_handle_ = nullptr;
  static std::pair<uint32_t, uint32_t> WatchInvalidated(void*, uint32_t start, uint32_t length, bool exact) {
    if (length == 0 || start >= 0x20000000u) return {0u, UINT32_MAX};
    length = std::min(length, 0x20000000u - start);
    uint32_t first = start >> 12, last = (start + length - 1) >> 12;
    if (!exact) {
      first &= ~63u;
      last |= 63u;
      if (last >= 0x20000u) last = 0x20000u - 1;
    }
    for (uint32_t p = first; p <= last; p++) {
      rex_nx_dirty[p] = 1;
      rex_nx_dirty_coarse[p >> 8] = 1;
    }
    return {first << 12, (last - first + 1) << 12};
  }
  void WatchRange(uint32_t physical, uint32_t bytes) {
#ifndef LEGOBATMAN_HASH_TEXTURES
    if (watch_handle_ && bytes) memory_->EnablePhysicalMemoryAccessCallbacks(physical & 0x1FFFFFFFu, bytes, true, false);
#else
    (void)physical; (void)bytes;
#endif
  }

  bool SetupContext() override {
#ifndef LEGOBATMAN_HASH_TEXTURES
    watch_handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(&WatchInvalidated, this);
#endif
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = DebugCallback;
    device_ = dkDeviceCreate(&dm);
    if (!device_) { Log("dkDeviceCreate failed"); return false; }
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device_);
    qm.flags = DkQueueFlags_Graphics;
    queue_ = dkQueueCreate(&qm);
    if (!queue_) { Log("dkQueueCreate failed"); return false; }
    // The CPU rewrites the descriptors, the frame data ring (uniforms, vertices, indices) and the staging ring at the same
    // addresses every second frame. With GpuCached the GPU's L2 can still hold the old lines (the CPU's uncached writes go
    // to DRAM, nothing invalidates the L2): stale texture headers, constants and upload bytes = the console-only row
    // streaks, wrong glyphs and flicker (Eden has no such cache: the same frames are clean there). GpuUncached reads
    // them from DRAM. The file sdmc:/switch/cvhd/gpucached restores the 0.258 flags for an A/B.
    const bool gpu_cached = access("sdmc:/switch/legobatman/gpucached", F_OK)  /* LEGO Batman port */ == 0;
    target_compression_ = access("sdmc:/switch/legobatman/nocompress", F_OK) != 0;
    std::printf("[nx] gpu: render target compression %s (file nocompress)\n", target_compression_ ? "on" : "off");
    // LEGO Batman port: sdmc:/switch/legobatman/genericvs draws every vertex shader that has a generic (disc)
    // translation with it, even when the exact one is in the romfs: tests the generic path on known scenes.
    prefer_generic_vs_ = access("sdmc:/switch/legobatman/genericvs", F_OK) == 0;
    if (prefer_generic_vs_) std::printf("[nx] gpu: generic vertex shaders preferred (file genericvs)\n");
    const uint32_t ring_flags = DkMemBlockFlags_CpuUncached | (gpu_cached ? DkMemBlockFlags_GpuCached : DkMemBlockFlags_GpuUncached);
    std::printf("[nx] gpu: descriptors, frame data and staging %s (file sdmc:/switch/cvhd/gpucached)\n",
                gpu_cached ? "GPU-cached (0.258 behaviour)" : "GPU-uncached");
    if (!code_.Create(device_, kCodePoolBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code) ||
        !images_.Create(device_, kImagePoolBytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image) ||
        !scaled_images_.Create(device_, kScaledPoolBytes, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image) ||
        !descriptors_.Create(device_, DK_MEMBLOCK_ALIGNMENT * 4 * kFrames + DK_MEMBLOCK_ALIGNMENT * 2 + kSamplerCount * sizeof(DkSamplerDescriptor), ring_flags) ||
        !cmdmem_.Create(device_, kFrameCmdBytes * kFrames, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached)) {
      Log("memory block creation failed");
      return false;
    }
    for (uint32_t i = 0; i < kFrames; i++) {
      Frame& f = frames_[i];
      // LEGO Batman port (perf, 2026-11-10): the data ring (vertices, indices, uniforms) is GPU-cached. The GPU only
      // reads it, and every frame's command buffer starts with an L2 invalidation (BeginFrame), which drops the lines of
      // the frame that last used these addresses. Uncached, every vertex/index fetch went to DRAM: ~27 ms of GPU time
      // per frame remained with one-pixel draws. GPU-written data (timestamps, read-backs) stays in the uncached
      // staging ring: with the whole ring cached (file gpucached) the game stopped at swap #1.
      if (!f.data.Create(device_, kFrameDataBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached) ||
          !f.staging.Create(device_, kStagingBytes, ring_flags)) {
        Log("frame %u memory failed", i);
        return false;
      }
      DkCmdBufMaker cm;
      dkCmdBufMakerDefaults(&cm, device_);
      f.cmdbuf = dkCmdBufCreate(&cm);
      f.cmd_offset = i * kFrameCmdBytes;
      f.desc_offset = i * kMaxImageDescriptors * sizeof(DkImageDescriptor);
    }
    persist_ok_ = persist_.Create(device_, kPersistBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    std::printf("[nx] gpu: persistent vertex/index pool %s (%u MB)\n", persist_ok_ ? "on" : "FAILED (ring only)", kPersistBytes >> 20);
    sampler_desc_offset_ = kFrames * kMaxImageDescriptors * sizeof(DkImageDescriptor);
    // Samplers, indexed by SamplerIndex: the min and mag filters (the fetch instruction may override the
    // fetch constant's), the border colour (black / white) and every Xenos clamp mode per axis.
    for (uint32_t i = 0; i < kSamplerCount; i++) {
      DkSampler s;
      dkSamplerDefaults(&s);
      s.minFilter = (i & 1) ? DkFilter_Linear : DkFilter_Nearest;
      s.magFilter = (i & 2) ? DkFilter_Linear : DkFilter_Nearest;
      s.mipFilter = DkMipFilter_None;
      s.wrapMode[0] = WrapModeOf(xenos::ClampMode((i >> 3) & 7));
      s.wrapMode[1] = WrapModeOf(xenos::ClampMode((i >> 6) & 7));
      s.wrapMode[2] = DkWrapMode_ClampToEdge;
      float border = (i & 4) ? 1.0f : 0.0f;
      for (int c = 0; c < 4; c++) s.borderColor[c].value_f = border;
      DkSamplerDescriptor sd;
      dkSamplerDescriptorInitialize(&sd, &s);
      std::memcpy(descriptors_.cpu + sampler_desc_offset_ + i * sizeof(DkSamplerDescriptor), &sd, sizeof(sd));
    }
    // The swapchain on the default window.
    for (uint32_t i = 0; i < kFrames; i++) {
      HostImage* img = CreateImage(kPresentWidth, kPresentHeight, DkImageFormat_RGBA8_Unorm,
                                   DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression, false);
      if (!img) { Log("swapchain image %u failed", i); return false; }
      swap_images_[i] = img;
      swap_image_ptrs_[i] = &img->image;
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device_, nwindowGetDefault(), swap_image_ptrs_, kFrames);
    swapchain_ = dkSwapchainCreate(&sm);
    if (!swapchain_) { Log("dkSwapchainCreate failed"); return false; }
    shader_dir_ = REXCVAR_GET(deko3d_shader_dir);
    if (shader_dir_.empty() || shader_dir_.back() != '/') shader_dir_ += '/';
    size_t cut = shader_dir_.size() >= 2 ? shader_dir_.rfind('/', shader_dir_.size() - 2) : std::string::npos;
    std::string debug_dir = cut == std::string::npos ? "" : shader_dir_.substr(0, cut + 1);
    present_target_ = access((debug_dir + "dk_present_rt").c_str(), F_OK) == 0;
    dump_images_ = access((debug_dir + "dk_dump").c_str(), F_OK) == 0;  // readback dumps stall the GPU: debug only
    debug_dir_ = debug_dir;
    ReadFpsMode();
    trace_start_ms_ = NowMs();
    trace_enabled_ = access((debug_dir + "dk_trace").c_str(), F_OK) == 0;
    if (trace_enabled_) {
      if (FILE* tf = std::fopen((debug_dir + "dk_trace").c_str(), "r")) {  // optional: the first swap to trace
        unsigned n = 0;
        char word[8] = {};
        if (std::fscanf(tf, "%7[a-z]", word) == 1) { trace_on_mip_ = std::strcmp(word, "mip") == 0; trace_any_ = std::strcmp(word, "any") == 0; }  // mip: the first mip-only frame; any: no draw-count gate (menus)
        if (std::fscanf(tf, "%u", &n) == 1) trace_min_swap_ = n;
        if (std::fscanf(tf, "%u", &n) == 1) trace_second_ = n;  // optional: a second frame to trace, seconds (or, with a first time of 0, swaps after the first ring-full event)
        std::fclose(tf);
        if (trace_min_swap_ == 0 && trace_second_) { trace_after_full_ = trace_second_; trace_second_ = 0; }
      }
    }
    // LEGO Batman port: shaders packed in the NRO's romfs (romfs:/shaders/, CMakeLists.txt) win over the SD folder, so
    // a netloader deploy brings the matching shaders; missing-shader dumps and debug files stay next to the SD folder.
    load_dir_ = shader_dir_;
#ifndef CVHD_DK_GL
    if (R_SUCCEEDED(romfsInit()) && access("romfs:/shaders/present.vert.dksh", F_OK) == 0) load_dir_ = "romfs:/shaders/";
#endif
    present_vs_ = LoadDksh(load_dir_ + "present.vert.dksh", present_vs_shader_);
    present_fs_ = LoadDksh(load_dir_ + "present.frag.dksh", present_fs_shader_);
    Log("deko3d backend up: shaders from %s (%s present shaders), %u MB images, %u MB data per frame",
        load_dir_.c_str(), present_vs_ && present_fs_ ? "with" : "WITHOUT", kImagePoolBytes >> 20, kFrameDataBytes >> 20);
    BeginFrame();
    return true;
  }

  void ShutdownContext() override {
    if (queue_) dkQueueWaitIdle(queue_);
    if (watch_handle_) {
      memory_->UnregisterPhysicalMemoryInvalidationCallback(watch_handle_);
      watch_handle_ = nullptr;
    }
    Log("deko3d backend down: %u swaps, %u draws, %u skipped, %u without shaders, %u clears, %u resolves, %u images (%u MB), %u uploads (%u KB, %u re-uploads), %llu dirty pages",
        swaps_, draws_, skipped_, missing_draws_, clears_, resolves_, image_count_, images_.peak >> 20, uploads_,
        uint32_t(upload_bytes_ >> 10), reuploads_, (unsigned long long)dirty_pages_);
  }

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address, const uint32_t* host_address,
                     uint32_t dword_count) override {
    // LEGO Batman port (perf): the game sets a shader before most draws and UcodeHash read its whole microcode each
    // time. Shaders by (guest address, size), confirmed by eight microcode dwords, skip the hash.
    const uint64_t qkey = (uint64_t(guest_address) << 20) ^ dword_count;
    uint32_t probe[8];
    for (uint32_t k = 0; k < 8; k++) probe[k] = dword_count ? host_address[(uint64_t(dword_count - 1) * k) / 7] : 0;
    if (auto q = shader_quick_.find(qkey); q != shader_quick_.end() && std::memcmp(q->second.probe, probe, sizeof(probe)) == 0)
      return q->second.shader;
    uint64_t hash = cvhd::UcodeHash(host_address, dword_count);
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) {
      auto& qe = shader_quick_[qkey];
      qe.shader = it->second;
      std::memcpy(qe.probe, probe, sizeof(probe));
      return it->second;
    }
    DkShaderObj* shader = new DkShaderObj(shader_type, hash, host_address, dword_count);
    string::StringBuffer disasm;
    shader->AnalyzeUcode(disasm);
    shaders_.emplace(hash, shader);
    {
      auto& qe = shader_quick_[qkey];
      qe.shader = shader;
      std::memcpy(qe.probe, probe, sizeof(probe));
    }
    char name[512];
    std::snprintf(name, sizeof(name), "%sshader_%016llX.%s.dksh", load_dir_.c_str(), (unsigned long long)hash,
                  shader_type == xenos::ShaderType::kVertex ? "vert" : "frag");
    if (prefer_generic_vs_ && shader_type == xenos::ShaderType::kVertex) LoadGenericVs(*shader, host_address, dword_count);
    if (!shader->loaded) shader->loaded = LoadDksh(name, shader->shader);
    if (shader->loaded && !shader->generic && shader_type == xenos::ShaderType::kVertex) {
      std::snprintf(name, sizeof(name), "%sshader_%016llX.geom.dksh", load_dir_.c_str(), (unsigned long long)hash);
      shader->geom_loaded = LoadDksh(name, shader->geom);
      if (!shader->geom_loaded) LogOnce("geom", "no rectangle-list geometry shader for VS %016llX (%s)", (unsigned long long)hash, name);
    }
    if (!shader->loaded && shader_type == xenos::ShaderType::kVertex) LoadGenericVs(*shader, host_address, dword_count);
    if (!shader->loaded) {
      missing_shaders_++;
      Log("no DKSH for %s %016llX (%u dwords at %08X): dumping the ucode to %smissing/",
          shader_type == xenos::ShaderType::kVertex ? "VS" : "PS", (unsigned long long)hash, dword_count, guest_address,
          shader_dir_.c_str());
      DumpMissing(*shader);
    } else {
      REXLOG_INFO("[dk] shader {} {:016X} loaded ({} dwords)", shader_type == xenos::ShaderType::kVertex ? "VS" : "PS",
                  hash, dword_count);
    }
    return shader;
  }

  // LEGO Batman port: the fetch-agnostic key of a vertex shader (tools/scripts/vs_generic_key.py, which names the
  // generic shaders): FNV-1a 64 over the big-endian microcode with every vfetch instruction zeroed, then the sorted
  // (destination register, write mask) of the vfetches.
  static uint64_t GenericVsKey(const uint32_t* host, uint32_t count) {
    std::vector<uint32_t> w(count);
    for (uint32_t i = 0; i < count; i++) w[i] = __builtin_bswap32(host[i]);  // guest (big-endian) dwords
    std::vector<uint32_t> slots;
    uint32_t limit = count;
    for (uint32_t i = 0; i + 2 < count && i < limit; i += 3) {
      const uint32_t w0 = w[i], w1 = w[i + 1], w2 = w[i + 2];
      const uint32_t cf[2][4] = {{(w1 >> 12) & 15, w0 & 0xFFF, (w0 >> 12) & 7, (w0 >> 16) & 0xFFF},
                                 {w2 >> 28, (w1 >> 16) & 0xFFF, (w1 >> 28) & 7, w2 & 0xFFF}};
      for (const auto& c : cf) {
        const uint32_t op = c[0], addr = c[1], n = c[2], seq = c[3];
        if (!((op >= 1 && op <= 6) || op == 13 || op == 14) || !n) continue;
        limit = std::min(limit, addr * 3);
        for (uint32_t k = 0; k < n; k++) {
          const uint32_t at = addr + k;
          if (((seq >> (2 * k)) & 1) && uint64_t(at) * 3 + 2 < count && (w[at * 3] & 0x1F) == 0) slots.push_back(at);
        }
      }
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
    std::vector<uint16_t> fetches;
    for (uint32_t at : slots) {
      const uint32_t reg = (w[at * 3] >> 12) & 0x3F, swz = w[at * 3 + 1] & 0xFFF;
      uint32_t mask = 0;
      for (int c = 0; c < 4; c++)
        if (((swz >> (3 * c)) & 7) != 7) mask |= 1u << c;
      fetches.push_back(uint16_t(reg << 8 | mask));
      w[at * 3] = w[at * 3 + 1] = w[at * 3 + 2] = 0;
    }
    std::sort(fetches.begin(), fetches.end());
    uint64_t h = 1469598103934665603ull;
    auto byte = [&h](uint8_t b) { h ^= b; h *= 1099511628211ull; };
    for (uint32_t v : w) {
      byte(uint8_t(v >> 24)); byte(uint8_t(v >> 16)); byte(uint8_t(v >> 8)); byte(uint8_t(v));
    }
    for (uint16_t fm : fetches) { byte(uint8_t(fm >> 8)); byte(uint8_t(fm)); }
    return h;
  }

  void LoadGenericVs(DkShaderObj& shader, const uint32_t* host, uint32_t count) {
    const uint64_t key = GenericVsKey(host, count);
    char name[512];
    std::snprintf(name, sizeof(name), "%sshader_%016llX.gmap", load_dir_.c_str(), (unsigned long long)key);
    FILE* fp = std::fopen(name, "rb");
    if (!fp) return;
    uint8_t map[2 * DkShaderObj::kMaxGenericInputs];
    const size_t got = std::fread(map, 1, sizeof(map), fp);
    std::fclose(fp);
    const uint32_t n = uint32_t(got / 2);
    // Runtime fetches -> generic inputs by (register, write mask), in order where several share them.
    std::vector<bool> used(n, false);
    shader.gen_loc.clear();
    uint32_t unmatched = 0;
    for (const auto& vb : shader.vertex_bindings()) {
      for (const auto& a : vb.attributes) {
        const auto& r = a.fetch_instr.result;
        int8_t loc = -1;
        for (uint32_t k = 0; k < n; k++) {
          if (!used[k] && map[2 * k] == r.storage_index && map[2 * k + 1] == r.original_write_mask) {
            loc = int8_t(k);
            used[k] = true;
            for (int c = 0; c < 4; c++) shader.gen_swz[k][c] = int32_t(r.components[c]);
            break;
          }
        }
        if (loc < 0) unmatched++;
        shader.gen_loc.push_back(loc);
      }
    }
    for (uint32_t k = 0; k < n; k++) {
      if (!used[k]) {  // no runtime fetch: (0, 0, 0, 1) from a fixed attribute
        for (int c = 0; c < 4; c++) shader.gen_swz[k][c] = c == 3 ? 5 : 4;
        unmatched++;
      }
    }
    std::snprintf(name, sizeof(name), "%sshader_%016llX.gvert.dksh", load_dir_.c_str(), (unsigned long long)key);
    if (!LoadDksh(name, shader.shader)) return;
    shader.loaded = true;
    shader.generic = true;
    shader.gen_count = n;
    std::snprintf(name, sizeof(name), "%sshader_%016llX.ggeom.dksh", load_dir_.c_str(), (unsigned long long)key);
    shader.geom_loaded = LoadDksh(name, shader.geom);
    generic_shaders_++;
    Log("VS %016llX: generic shader %016llX (%u inputs%s)", (unsigned long long)shader.ucode_data_hash(),
        (unsigned long long)key, n, unmatched ? ", some unmatched" : "");
  }

  // Time spent in the backend per second (printed with the fps window): draws and resolves, the swap, and
  // the two waits that tell a GPU-bound frame from a CPU-bound one.
  static double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
                 bool major_mode_explicit) override {
    const double t0 = NowMs();
    const bool ok = IssueDrawImpl(prim_type, index_count, index_buffer_info, major_mode_explicit);
    t_draw_ms_ += NowMs() - t0;
    return ok;
  }
  bool IssueDrawImpl(xenos::PrimitiveType prim_type, uint32_t index_count, IndexBufferInfo* index_buffer_info,
                     bool major_mode_explicit) {
    FlushResolveBarrier();
    if (Skipping()) return true;
    // Phase timers (the dk time/s line): sync, setup, textures, targets+state, vertices, uniforms, draw.
    double phase_t = NowMs();
    auto phase = [&](int i) { const double n = NowMs(); t_phase_ms_[i] += n - phase_t; phase_t = n; };
    SyncDirtyPages();
    phase(0);
    const RegisterFile& regs = *register_file_;
    auto mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
    if (mode == xenos::EdramMode::kCopy) return Resolve();
    auto* vs = static_cast<DkShaderObj*>(active_vertex_shader());
    auto* ps = static_cast<DkShaderObj*>(active_pixel_shader());
    if (!vs || !ps) { LogOnce(vs ? "skip0ps" : "skip0vs", "draw skipped: no %s shader object (prim %u n=%u)", vs ? "pixel" : "vertex", uint32_t(prim_type), index_count); SkipDraw(0); return true; }
    auto mask = regs.Get<reg::RB_COLOR_MASK>();
    auto dc = draw_util::GetNormalizedDepthControl(regs);
    bool writes_color = mask.write_red0 || mask.write_green0 || mask.write_blue0 || mask.write_alpha0;
    bool writes_depth = dc.z_enable && dc.z_write_enable;
    if (!writes_color && !writes_depth && !dc.stencil_enable) {
      char k[64]; std::snprintf(k, sizeof(k), "skip1 %u %u %u", uint32_t(prim_type), dc.z_enable, uint32_t(mode));
      LogOnce(k, "draw skipped: writes nothing (colour mask 0, z %u/%u, prim %u n=%u, edram mode %u)", dc.z_enable, dc.z_write_enable, uint32_t(prim_type), index_count, uint32_t(mode));
      SkipDraw(1); return true; }
    if (IsClearDraw(vs, ps, prim_type)) {
      // Single-pass tiles: a lower tile's clear would wipe what the extended top-tile draws already put there.
      const auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
      if (SinglePass() && (wo.window_y_offset != 0 || wo.window_x_offset != 0)) return true;
      return ClearDraw(index_count, mode);
    }
#ifndef CVHD_DK_GL
    for (auto& slot : lbshot::skip_ps)
      if (slot.load() == ps->ucode_data_hash()) { SkipDraw(0); return true; }
    // "flag 1 1" (measurement only): skip the draws of the second EDRAM tile (window offset y != 0).
    if (lbshot::flags[1] && regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset != 0) { SkipDraw(0); return true; }
#endif
    const auto win_off = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    const bool bottom_tile = win_off.window_y_offset != 0 || win_off.window_x_offset != 0;
    if (bottom_tile) {
      tiling_seen_ = true;
      frame_offset_draws_ = true;
      const uint32_t off = uint32_t(-std::min(0, int(win_off.window_y_offset)));
      if (off && (!frame_tile_h_ || off < frame_tile_h_)) frame_tile_h_ = off;
    }
    // A new tile starts when the window offset changes; until its first tile-sized resolve its draws are the scene
    // (single-passed), after it the tile's post pass (drawn per tile; see Resolve).
    if (int(win_off.window_y_offset) != cur_tile_off_) {
      cur_tile_off_ = int(win_off.window_y_offset);
      tile_post_ = false;
    }
    const bool single_pass = SinglePass();
    uint64_t draw_key = 0;
    if (single_pass) {
      draw_key = DrawKey(regs, vs, ps, prim_type, index_count, index_buffer_info);
      if (bottom_tile && !tile_post_ && tile_keys_.count(draw_key)) {
        frame_tile_dups_++;
        if (FrameLogOn()) Log("frm #%u dup PS %08X wo %d", swaps_ + 1, uint32_t(ps->ucode_data_hash()), int(win_off.window_y_offset));
        return true;  // already drawn with the full scissor in the top tile
      }
    }
    if (!vs->loaded || !ps->loaded) { missing_draws_++; frame_missing_++; return true; }
    DkPrimitive prim;
    bool rects;
    if (!PrimitiveOf(prim_type, prim, rects)) { LogOnce("prim", "unsupported primitive type %u", uint32_t(prim_type)); SkipDraw(2); return true; }
    Frame& f = frames_[frame_];
    DkCmdBuf cmd = f.cmdbuf;
    const uint64_t pair_key = vs->ucode_data_hash() * 0x9E3779B97F4A7C15ull ^ ps->ucode_data_hash();
    if (FirstTime(pair_key)) {
      // Each shader pair's first draw, once: the state a new kind of draw comes with.
      auto blend = regs.Get<reg::RB_BLENDCONTROL>();
      {
        Log("draw VS %016llX PS %016llX: prim %u n=%u%s, blend %u/%u op %u, mask %x, z %u/%u func %u, cull f%u b%u face %u, %zu vfetch %zu tfetch",
                (unsigned long long)vs->ucode_data_hash(), (unsigned long long)ps->ucode_data_hash(), uint32_t(prim_type), index_count,
                index_buffer_info ? " indexed" : "", uint32_t(blend.color_srcblend), uint32_t(blend.color_destblend),
                uint32_t(blend.color_comb_fcn), regs.values[XE_GPU_REG_RB_COLOR_MASK] & 0xF, dc.z_enable, dc.z_write_enable,
                uint32_t(dc.zfunc), regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front, regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_back, regs.Get<reg::PA_SU_SC_MODE_CNTL>().face, vs->vertex_bindings().size(), ps->texture_bindings().size());
        // Its vertex layout and the first vertices (byte-swapped words as hex and as floats).
        for (const auto& vb : vs->vertex_bindings()) {
          auto fetch = regs.GetVertexFetch(vb.fetch_constant);
          std::string attrs;
          for (const auto& a : vb.attributes) {
            char s[64];
            std::snprintf(s, sizeof(s), " [fmt %u off %u%s%s]", uint32_t(a.fetch_instr.attributes.data_format), a.fetch_instr.attributes.offset,
                          a.fetch_instr.attributes.is_signed ? " signed" : "", a.fetch_instr.attributes.is_integer ? " integer" : "");
            attrs += s;
          }
          const uint32_t* w = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(fetch.address << 2));
          uint32_t n = std::min<uint32_t>(vb.stride_words * 3, 24);
          std::string words;
          for (uint32_t i = 0; i < n && i < fetch.size; i++) {
            uint32_t v = fetch.endian == xenos::Endian::k8in32 ? __builtin_bswap32(w[i]) : w[i];
            float fl;
            std::memcpy(&fl, &v, 4);
            char s[48];
            std::snprintf(s, sizeof(s), " %08X(%g)", v, fl);
            words += s;
          }
          Log("  vf%u stride %u words, %u words at %08X endian %u:%s; data%s", vb.fetch_constant, vb.stride_words, fetch.size, fetch.address << 2,
              uint32_t(fetch.endian), attrs.c_str(), words.c_str());
        }
      }
    }

    phase(1);
    // Textures of the pixel shader (before the render targets: a new image is cleared on creation, which
    // rebinds the targets).
    std::string bound_textures;  // for the draw-type log below (built only when the trace or a first draw needs it)
    struct TexName { uint32_t fc, base, w, h; } tnames[32];
    uint32_t tcount = 0;
    uint64_t texset_key = pair_key;
    bool textures_ok = true;
    bool reads_tile_resolve = false;  // samples an image resolved tile by tile this frame (single-pass tiles)
    // LEGO Batman port: textures of both stages; vertex shaders fetch textures too (vertex texture fetch: first seen
    // in VS 8A67D3A2495FEAFD, tf16). The pixel stage first, as before.
    for (int tpass = 0; tpass < 2; tpass++) {
    DkShaderObj* const tsh = tpass ? vs : ps;
    const DkStage tstage = tpass ? DkStage_Vertex : DkStage_Fragment;
    uint32_t tb_index = 0;
    for (const auto& tb : tsh->texture_bindings()) {
      auto fetch = regs.GetTextureFetch(tb.fetch_constant);
      const uint32_t* fw = reinterpret_cast<const uint32_t*>(&fetch);
      uint64_t bind_key = tsh->ucode_data_hash() ^ (uint64_t(tb_index++) * 0x9E3779B97F4A7C15ull);
      for (int i = 0; i < 6; i++) bind_key = (bind_key ^ fw[i]) * 0x100000001B3ull;
      if (auto it = f.texbinds.find(bind_key); it != f.texbinds.end()) {
        Frame::TexBind& c = it->second;
        HostImage* tex = c.tex;
        bool valid = tex->guest_base == c.base && tex->width == c.width && tex->height == c.height && tex->from_resolve == c.from_resolve;
        if (valid && tex->checked_serial != serial_) {
#ifdef LEGOBATMAN_HASH_TEXTURES
          // LEGO Batman port: one change check per image per frame (the serial advances per draw; hashing at every
          // cached bind cost ~300 ms/s at ~40,000 binds/s).
          if (tex->checked_frame == swaps_) tex->checked_serial = serial_;
          else if (tex->checked_frame = swaps_, MemoryChanged(*tex, c.base, c.bytes)) { valid = false; tex->checked_frame = ~0u; }
          else tex->checked_serial = serial_;
#else
          if (MemoryChanged(*tex, c.base, c.bytes)) valid = false; else tex->checked_serial = serial_;
#endif
        }
        if (valid) {
          dkCmdBufBindTextures(cmd, tstage, tb.fetch_constant, &c.handle, 1);
          texset_key = (texset_key ^ c.base) * 0x100000001B3ull + tb.fetch_constant;
          if (tcount < 32) tnames[tcount++] = TexName{tb.fetch_constant, c.base, tex->width, tex->height};
          texbind_hits_++;
          continue;
        }
        f.texbinds.erase(it);
      }
      TextureInfo info;
      bool cube_fallback = false;
      // 1D textures are 2D textures of height 1 on the Xenos (the shaders sample them so).
      if (!TextureInfo::Prepare(fetch, &info) ||
          (info.dimension != xenos::DataDimension::k2DOrStacked && info.dimension != xenos::DataDimension::k1D)) {
        LogOnce("tex", "texture fetch %u not preparable or not 1D/2D (dim %u)", tb.fetch_constant, uint32_t(fetch.dimension));
        // LEGO Batman port: cube maps (reflections) get a black placeholder instead of dropping the draw.
        if (fetch.dimension == xenos::DataDimension::kCube && PlaceholderTexture(false)) {
          cube_fallback = true;
        } else {
          textures_ok = false;
          continue;
        }
      }
      HostImage* tex = cube_fallback ? PlaceholderTexture(false) : TextureImage(f, info, fetch);
      if (!tex) { textures_ok = false; continue; }  // e.g. the staging ring is full this frame: draw next frame
      // Only while the picture is still being resolved tile by tile: the frame's later full-screen passes read the
      // completed image and are drawn once.
      if (tex->tile_resolved == swaps_ + 1 && tex->tile_rows < tex->gh) reads_tile_resolve = true;
      if (tex->from_resolve && FrameLogOn())
        Log("frm #%u fetch tf%u %08X img %05X %ux%u by PS %08X", swaps_ + 1, tb.fetch_constant, info.memory.base_address, ImgId(tex),
            tex->width, tex->height, uint32_t(tsh->ucode_data_hash()));
      uint32_t swizzle = tex->from_resolve ? 0 : fetch.swizzle;
      if (swizzle && tex->reversed_channels) {
        uint32_t r = 0;
        for (uint32_t i = 0; i < 4; i++) {
          uint32_t c = (swizzle >> (3 * i)) & 7;
          r |= (c < 4 ? 3 - c : c) << (3 * i);
        }
        swizzle = r;
      }
      uint32_t slot = DescriptorFor(f, tex, swizzle);
      if (slot == UINT32_MAX) { textures_ok = false; continue; }
      uint32_t sampler = SamplerIndex(fetch, tb.fetch_instr);
      if (FirstTime((uint64_t(info.memory.mip_address) << 32) ^ info.memory.base_address ^ 0x7465780000000000ull)) {
        // Each texture's first bind, once: what the game draws with (correlates with the uploads).
        Log("texture at %08X: %s %ux%u %s endian %u swizzle %03X filters min %u mag %u (instr %u/%u) clamp %u/%u (%u KB) mips %u-%u at %08X%s",
                info.memory.base_address, info.format_info()->name, info.width + 1, info.height + 1, info.is_tiled ? "tiled" : "linear",
                uint32_t(fetch.endianness), fetch.swizzle, uint32_t(fetch.min_filter), uint32_t(fetch.mag_filter),
                uint32_t(tb.fetch_instr.attributes.min_filter), uint32_t(tb.fetch_instr.attributes.mag_filter), uint32_t(fetch.clamp_x),
                uint32_t(fetch.clamp_y), info.memory.base_size >> 10, info.mip_min_level, info.mip_max_level, info.memory.mip_address,
                tex->from_resolve ? " (a resolve image)" : "");
      }
      // LEGO Batman port (console movie flicker hunt): for the movie's YUV draws, per plane: the swap, the plane, whether
      // this bind uploaded it, and whether guest memory still equals what was uploaded (a full hash now vs the upload's).
      if (tpass == 0 && ps->ucode_data_hash() == 0x047DC05346A835EAull && vid_log_ < 900) {
        vid_log_++;
        const uint64_t now = HashMemory(info.memory.base_address, tex->mem_bytes, true);
        Log("vid swap #%u tf%u %08X up %u same %u", swaps_ + 1, tb.fetch_constant, info.memory.base_address,
            tex->content_serial == serial_ ? 1u : 0u, now == tex->mem_hash ? 1u : 0u);
      }
      DkResHandle handle = dkMakeTextureHandle(slot, sampler);
      dkCmdBufBindTextures(cmd, tstage, tb.fetch_constant, &handle, 1);
      f.texbinds[bind_key] = Frame::TexBind{tex, handle, tex->guest_base, tex->mem_bytes, tex->width, tex->height, tex->from_resolve};
      texset_key = (texset_key ^ info.memory.base_address) * 0x100000001B3ull + tb.fetch_constant;
      if (tcount < 32) tnames[tcount++] = TexName{tb.fetch_constant, info.memory.base_address, tex->width, tex->height};
    }
    }  // tpass
    const bool first_texset = FirstTime(texset_key ^ 0x7061697274657800ull);
    if (first_texset || (trace_enabled_ && !traced_)) {
      for (uint32_t i = 0; i < tcount; i++) {
        char t[48];
        std::snprintf(t, sizeof(t), " tf%u=%08X(%ux%u)", tnames[i].fc, tnames[i].base, tnames[i].w, tnames[i].h);
        bound_textures += t;
      }
    }
    if (first_texset) {
      // Each shader pair's first draw with each texture set, once (the texture bases name the layer).
      Log("draw VS %016llX PS %016llX prim %u n=%u:%s", (unsigned long long)vs->ucode_data_hash(),
          (unsigned long long)ps->ucode_data_hash(), uint32_t(prim_type), index_count, bound_textures.empty() ? " no textures" : bound_textures.c_str());
    }
    // A draw with a missing texture would sample whatever the slot held before (noise): skip it.
    if (!textures_ok) { SkipDraw(3); return true; }
    phase(2);

    // Render targets.
    auto surf = regs.Get<reg::RB_SURFACE_INFO>();
    auto color_info = regs.Get<reg::RB_COLOR_INFO>();
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    uint32_t pitch = EffectivePitch(surf);
    HostImage* color = (mode != xenos::EdramMode::kDepthOnly && writes_color)
                           ? TargetImage(color_info.color_base, pitch, ColorTargetFormatOf(color_info.color_format), false)
                           : nullptr;
    HostImage* depth = (dc.z_enable || dc.stencil_enable) ? TargetImage(depth_info.depth_base, pitch, DkImageFormat_Z24S8, true) : nullptr;
    if (!color && !depth) { SkipDraw(4); return true; }
    BindTargets(cmd, color, depth);
    // Guest pixels (the render scale is applied when the viewport and scissor are set).
    const HostImage* timg = color ? color : depth;
    uint32_t target_w = timg->gw;
    uint32_t target_h = timg->gh;

    // Viewport and scissor (the window offset included, as the SDK computes them).
    // deko3d (upper-left origin) maps NDC y = +1 to the bottom row like Vulkan; the Xenos clip space is
    // y-up like Direct3D, so the SDK's "origin_bottom_left" negation of the NDC y scale/offset applies
    // (the first Eden picture was upside down without it).
    draw_util::ViewportInfo vi;
    draw_util::GetHostViewportInfo(regs, 1, 1, true, target_w, target_h, false, dc, false, false, false, vi);
    DkViewport vp{float(vi.xy_offset[0]), float(vi.xy_offset[1]), float(vi.xy_extent[0]), float(vi.xy_extent[1]), vi.z_min, vi.z_max};
    if (single_pass && bottom_tile) {
      // Drawn at its final rows, not the tile's: D3D moves a tile's geometry by the window offset, through
      // vtx_window_offset_enable or through the viewport offset registers it rewrites per tile (both seen).
      vp.x -= float(win_off.window_x_offset);
      vp.y -= float(win_off.window_y_offset);
    }
    if (timg->hs != kScaleDen) {
      const float k = float(timg->hs) / float(kScaleDen);
      vp.x *= k; vp.y *= k; vp.width *= k; vp.height *= k;
    }
    dkCmdBufSetViewports(cmd, 0, &vp, 1);
    draw_util::Scissor sc;
    draw_util::GetScissor(regs, sc, true);
    DkScissor scissor{sc.offset[0], sc.offset[1], sc.extent[0], sc.extent[1]};
    if (single_pass) {
      // The window scissor without the window offset: its rows on the final picture. (The screen scissor, which
      // only tiling sets, is dropped.)
      const auto tl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
      const auto br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
      scissor = DkScissor{tl.tl_x, tl.tl_y, br.br_x > tl.tl_x ? br.br_x - tl.tl_x : 0u, br.br_y > tl.tl_y ? br.br_y - tl.tl_y : 0u};
      // LEGO Batman port: a draw sampling an image resolved tile by tile this frame (the per-tile post effect after each
      // tile's resolve) needs that tile's rows only: drawn once per tile at its final rows, never extended or skipped.
      if ((reads_tile_resolve || tile_post_) && tile_h_) {
        const uint32_t y0 = bottom_tile ? uint32_t(-std::min(0, int(win_off.window_y_offset))) : 0u;
        const uint32_t top = std::max(scissor.y, y0), bot = std::min(scissor.y + scissor.height, y0 + tile_h_);
        scissor.y = top;
        scissor.height = bot > top ? bot - top : 0;
        frame_tile_dependent_++;
      } else
      // A top-tile draw clipped at the tile's bottom covers the whole picture: its copies in the lower tiles are
      // skipped as duplicates.
      if (!bottom_tile && tile_h_ && scissor.y + scissor.height == tile_h_) {
        const float vb = vp.y + vp.height;
        const uint32_t bottom = uint32_t(std::max(0.0f, std::min(float(target_h), vb)));
        if (bottom > scissor.y + scissor.height) scissor.height = bottom - scissor.y;
      }
      // A top-tile draw whose scissor now reaches below the tile makes its lower-tile copies redundant.
      if (!reads_tile_resolve && !tile_post_ && !bottom_tile && tile_h_ && scissor.y + scissor.height > tile_h_) tile_keys_.insert(draw_key);
    }
    if (scissor.x + scissor.width > target_w) scissor.width = scissor.x < target_w ? target_w - scissor.x : 0;
    if (scissor.y + scissor.height > target_h) scissor.height = scissor.y < target_h ? target_h - scissor.y : 0;
#ifndef CVHD_DK_GL
    // Debug server "flag 5 1": one pixel per draw (measurement only). GPU time that remains is vertex/setup/command work.
    if (lbshot::flags[5]) { scissor.width = scissor.width ? 1 : 0; scissor.height = scissor.height ? 1 : 0; }
#endif
    if (timg->hs != kScaleDen) scissor = ScaleRect(scissor.x, scissor.y, scissor.width, scissor.height, timg);
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    if (FrameLogOn())
      Log("frm #%u draw PS %08X wo %d,%d tile_h %u vp y %.0f h %.0f sc y %u h %u key %016llX", swaps_ + 1, uint32_t(ps->ucode_data_hash()),
          int(win_off.window_x_offset), int(win_off.window_y_offset), tile_h_, vp.y, vp.height, scissor.y, scissor.height,
          (unsigned long long)draw_key);

    // Fixed-function state.
    auto su = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    // Rectangle lists: the geometry shader makes the 4th vertex, or the hardware mode (cvar).
    const bool use_gs = rects && vs->geom_loaded && !REXCVAR_GET(deko3d_fill_rectangle);
    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.cullMode = DkFace_None;
    // Face culling for triangles: the book's page turn draws the page mesh twice (front faces with the
    // front texture, back faces with the mirrored one); without culling the left page came out mirrored
    // (console 0.141). Xenos face 0 = front is CCW as seen on screen; the cvar inverts it if the picture says so.
    if (REXCVAR_GET(deko3d_cull) != 0 && (prim_type == xenos::PrimitiveType::kTriangleList ||
                                          prim_type == xenos::PrimitiveType::kTriangleStrip ||
                                          prim_type == xenos::PrimitiveType::kTriangleFan ||
                                          prim_type == xenos::PrimitiveType::kQuadList || prim_type == xenos::PrimitiveType::kQuadStrip)) {
      rs.cullMode = su.cull_front ? (su.cull_back ? DkFace_FrontAndBack : DkFace_Front)
                                  : (su.cull_back ? DkFace_Back : DkFace_None);
      const bool front_cw = (su.face != 0) != (REXCVAR_GET(deko3d_cull) == 2);
      rs.frontFace = front_cw ? DkFrontFace_CW : DkFrontFace_CCW;
    }
    rs.fillRectangleEnable = rects && !use_gs;
    // LEGO Batman port: depth bias (polygon offset), converted as the SDK's Vulkan path does. Without it the game's
    // decals and overlays drawn on top of coplanar surfaces z-fought: flicker on the console's GPU.
    {
      float slope = 0.0f, constant = 0.0f;
      draw_util::GetPreferredFacePolygonOffset(regs, draw_util::IsPrimitivePolygonal(regs), slope, constant);
      constant *= regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
                      ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                      : draw_util::kD3D10PolygonOffsetFactorFloat24;
      slope *= xenos::kPolygonOffsetScaleSubpixelUnit;
      if (depth && (constant != 0.0f || slope != 0.0f)) {
        rs.depthBiasEnableMask = DkPolygonFlag_All;
        const float bias[2] = {constant, slope};
        if (StateChanged(kSDepthBias, bias, sizeof(bias))) dkCmdBufSetDepthBias(cmd, constant, 0.0f, slope);
        frame_biased_++;
      }
    }
    if (StateChanged(kSRast, &rs, sizeof(rs))) dkCmdBufBindRasterizerState(cmd, &rs);
    auto blend = regs.Get<reg::RB_BLENDCONTROL>();
    bool blend_enabled = !(blend.color_srcblend == xenos::BlendFactor::kOne && blend.color_destblend == xenos::BlendFactor::kZero &&
                           blend.color_comb_fcn == xenos::BlendOp::kAdd && blend.alpha_srcblend == xenos::BlendFactor::kOne &&
                           blend.alpha_destblend == xenos::BlendFactor::kZero && blend.alpha_comb_fcn == xenos::BlendOp::kAdd);
    DkColorState cs;
    dkColorStateDefaults(&cs);
    cs.blendEnableMask = blend_enabled ? 1 : 0;
    if (StateChanged(kSColor, &cs, sizeof(cs))) dkCmdBufBindColorState(cmd, &cs);
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    cw.masks = (mask.write_red0 ? DkColorMask_R : 0) | (mask.write_green0 ? DkColorMask_G : 0) |
               (mask.write_blue0 ? DkColorMask_B : 0) | (mask.write_alpha0 ? DkColorMask_A : 0);
    if (StateChanged(kSColorWrite, &cw, sizeof(cw))) dkCmdBufBindColorWriteState(cmd, &cw);
    DkBlendState bs{BlendOp(blend.color_comb_fcn), BlendFactor(blend.color_srcblend), BlendFactor(blend.color_destblend),
                    BlendOp(blend.alpha_comb_fcn), BlendFactor(blend.alpha_srcblend), BlendFactor(blend.alpha_destblend)};
    if (StateChanged(kSBlend, &bs, sizeof(bs))) dkCmdBufBindBlendStates(cmd, 0, &bs, 1);
    DkDepthStencilState ds;
    dkDepthStencilStateDefaults(&ds);
    ds.depthTestEnable = depth && dc.z_enable;
    ds.depthWriteEnable = depth && dc.z_write_enable;
    ds.depthCompareOp = CompareOp(dc.zfunc);
    // LEGO Batman port: stencil (was always off). Xenos stencil ops and compare functions are deko3d's minus one.
    ds.stencilTestEnable = depth && dc.stencil_enable;
    if (ds.stencilTestEnable) {
      auto op = [](xenos::StencilOp o) { return DkStencilOp(uint32_t(o) + 1); };
      ds.stencilFrontFailOp = op(dc.stencilfail);
      ds.stencilFrontPassOp = op(dc.stencilzpass);
      ds.stencilFrontDepthFailOp = op(dc.stencilzfail);
      ds.stencilFrontCompareOp = CompareOp(dc.stencilfunc);
      const bool bf = dc.backface_enable;
      ds.stencilBackFailOp = op(bf ? dc.stencilfail_bf : dc.stencilfail);
      ds.stencilBackPassOp = op(bf ? dc.stencilzpass_bf : dc.stencilzpass);
      ds.stencilBackDepthFailOp = op(bf ? dc.stencilzfail_bf : dc.stencilzfail);
      ds.stencilBackCompareOp = CompareOp(bf ? dc.stencilfunc_bf : dc.stencilfunc);
      auto rm = regs.Get<reg::RB_STENCILREFMASK>();
      reg::RB_STENCILREFMASK rmb;
      rmb.value = bf ? regs.values[XE_GPU_REG_RB_STENCILREFMASK_BF] : rm.value;
      const uint32_t st[2] = {rm.value, rmb.value};
      if (StateChanged(kSStencil, st, sizeof(st))) {
        dkCmdBufSetStencil(cmd, DkFace_Front, rm.stencilwritemask, rm.stencilref, rm.stencilmask);
        dkCmdBufSetStencil(cmd, DkFace_Back, rmb.stencilwritemask, rmb.stencilref, rmb.stencilmask);
      }
      frame_stenciled_++;
    }
    if (StateChanged(kSDepth, &ds, sizeof(ds))) dkCmdBufBindDepthStencilState(cmd, &ds);

    // Shaders (the whole graphics mask: a stage without a shader here is unbound).
    DkShader const* shaders[3] = {&vs->shader, &ps->shader, &vs->geom};
#ifndef CVHD_DK_GL
    // Debug server "flag 3 1": the alternative shader set (romfs:/shaders_alt/), e.g. another translator/uam variant.
    // "flag 3 N" (1-4): the set romfs:/shaders_altN/.
    if (const int set = lbshot::flags[3]; set >= 1 && set <= 4) {
      auto alt = [&](DkShaderObj* s) -> DkShader const* {
        const int k = set - 1;
        if (s->alt_state[k] == 0) {
          char name[512];
          std::snprintf(name, sizeof(name), "romfs:/shaders_alt%d/shader_%016llX.%s.dksh", set, (unsigned long long)s->ucode_data_hash(),
                        s->type() == xenos::ShaderType::kVertex ? "vert" : "frag");
          s->alt_state[k] = LoadDksh(name, s->alt[k]) ? 1 : -1;
        }
        return s->alt_state[k] == 1 ? &s->alt[k] : nullptr;
      };
      if (auto a = alt(vs)) shaders[0] = a;
      if (auto a = alt(ps)) shaders[1] = a;
    }
#endif
    {
      DkShader const* key[4] = {shaders[0], shaders[1], use_gs ? shaders[2] : nullptr, nullptr};
      if (StateChanged(kSShaders, key, sizeof(key))) dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, use_gs ? 3 : 2);
    }
    phase(3);

    // Vertex layout and data: one buffer per fetch constant the vertex shader uses.
    DkVtxAttribState attribs[DK_MAX_VERTEX_ATTRIBS];
    DkVtxBufferState buffers[DK_MAX_VERTEX_BUFFERS];
    uint32_t attrib_count = 0, buffer_count = 0, runtime_attr = 0;
    if (vs->generic) {  // inputs without a runtime fetch: fixed (0, 0, 0, 0), swizzled to (0, 0, 0, 1)
      for (uint32_t k = 0; k < vs->gen_count; k++) {
        attribs[k] = DkVtxAttribState{};
        attribs[k].isFixed = 1;
        attribs[k].size = DkVtxAttribSize_4x32;
        attribs[k].type = DkVtxAttribType_Float;
      }
    }
    uint32_t vertex_count = index_count;
    DkGpuAddr index_addr = 0;
    if (index_buffer_info) {
      if (!CopyIndices(f, *index_buffer_info, index_addr, vertex_count)) { SkipDraw(5); return true; }
    }
    for (const auto& vb : vs->vertex_bindings()) {
      if (buffer_count >= DK_MAX_VERTEX_BUFFERS) break;
      auto fetch = regs.GetVertexFetch(vb.fetch_constant);
      uint32_t stride = vb.stride_words * 4;
      uint32_t bytes = vertex_count * stride;
      uint32_t avail = fetch.size * 4;
      if (bytes > avail) {
        LogOnce("vbclamp", "vertex data clamped: %u vertices x %u bytes = %u, fetch constant %u holds %u bytes (VS %016llX)", vertex_count,
                stride, bytes, vb.fetch_constant, avail, (unsigned long long)vs->ucode_data_hash());
        bytes = avail;
      }
      const uint8_t* src = memory_->TranslatePhysical(fetch.address << 2);
      const auto vendian = fetch.endian;
      DkGpuAddr vaddr = PersistLookup(persist_vtx_, (uint64_t(fetch.address) << 2) | uint32_t(fetch.endian), src, bytes,
                                      [vendian](uint8_t* d, const uint8_t* sp, uint32_t n, uint32_t*) {
                                        texture_conversion::CopySwapBlock(vendian, d, sp, n);
                                      });
      if (!vaddr) {
        bool fresh = true;
        uint32_t off = CopyBlockOnce(f, fetch.address << 2, src, bytes, uint32_t(fetch.endian), &fresh);
        if (off == UINT32_MAX) { LogOnce("ring", "frame data ring full"); SkipDraw(6); return true; }
        if (fresh) texture_conversion::CopySwapBlock(fetch.endian, f.data.cpu + off, src, bytes);
        vaddr = f.data.gpu + off;
      }
      dkCmdBufBindVtxBuffer(cmd, buffer_count, vaddr, bytes);
      buffers[buffer_count] = DkVtxBufferState{stride, 0};
      for (const auto& a : vb.attributes) {
        const uint32_t ai = runtime_attr++;
        if (vs->generic) {  // LEGO Batman port: the generic shader's input for this fetch
          if (ai >= vs->gen_loc.size() || vs->gen_loc[ai] < 0) continue;
        } else if (attrib_count >= DK_MAX_VERTEX_ATTRIBS) {
          break;
        }
        DkVtxAttribSize size;
        DkVtxAttribType type;
        if (!VertexAttribOf(a.fetch_instr.attributes, size, type)) {
          LogOnce("vfmt", "unsupported vertex format %u", uint32_t(a.fetch_instr.attributes.data_format));
          size = DkVtxAttribSize_4x32;
          type = DkVtxAttribType_Float;
        }
        DkVtxAttribState st{};
        st.bufferId = buffer_count;
        st.isFixed = 0;
        st.offset = uint32_t(a.fetch_instr.attributes.offset) * 4;
        st.size = size;
        st.type = type;
        st.isBgra = 0;
        if (vs->generic) attribs[vs->gen_loc[ai]] = st;
        else attribs[attrib_count++] = st;
      }
      buffer_count++;
    }
    if (vs->generic) attrib_count = vs->gen_count;
    if (StateChanged(kSVtxAttr, attribs, attrib_count * sizeof(attribs[0])) || !attrib_count)
      dkCmdBufBindVtxAttribState(cmd, attribs, attrib_count);
    if (StateChanged(kSVtxBuf, buffers, buffer_count * sizeof(buffers[0])) || !buffer_count)
      dkCmdBufBindVtxBufferState(cmd, buffers, buffer_count);
    phase(4);

    // Uniforms: the ALU constants and the system constants, per stage.
    if (!BindUniforms(f, vs, ps, vi, regs)) { SkipDraw(7); return true; }
    phase(5);

    PsMark(f, ps->ucode_data_hash());
    if (index_buffer_info) {
      dkCmdBufBindIdxBuffer(cmd, index_buffer_info->format == xenos::IndexFormat::kInt32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16,
                            index_addr);
      dkCmdBufDrawIndexed(cmd, prim, index_count, DrawInstances(), 0, 0, 0);
    } else {
      dkCmdBufDraw(cmd, prim, index_count, DrawInstances(), 0, 0);
    }
    if (trace_enabled_ && !traced_) {
      // One line per draw of a gameplay frame (file sdmc:/switch/cvhd/dk_trace): what the HP/MP bar and the
      // scaled sprites are drawn with.
      std::string verts;
      if (!vs->vertex_bindings().empty()) {
        const auto& vb0 = vs->vertex_bindings().front();
        auto vf = regs.GetVertexFetch(vb0.fetch_constant);
        const uint32_t* w = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(vf.address << 2));
        auto half = [](uint16_t h) -> float {
          const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
          float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
          return s ? -v : v;
        };
        auto word = [&](uint32_t i) { return vf.endian == xenos::Endian::k8in32 ? __builtin_bswap32(w[i]) : w[i]; };
        if (vs->ucode_data_hash() == 0x9DC6ADB3E4AA3763ull && vb0.stride_words >= 5 && vf.size >= vb0.stride_words * 3) {
          // The sprite vertex shader: 4 halves position, 2 halves uv, RGBA8 colour. Screen position of corners 0 and 2.
          float c[16];
          for (int k = 0; k < 16; k++) c[k] = regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + 18 * 4 + k);
          for (uint32_t corner = 0; corner < 4 && corner * vb0.stride_words + 5 <= vf.size; corner += 2) {
            const uint32_t b = corner * vb0.stride_words;
            const float px = half(uint16_t(word(b))), py = half(uint16_t(word(b) >> 16)), pz = half(uint16_t(word(b + 1))),
                        pw = half(uint16_t(word(b + 1) >> 16));
            const float r3x = py, r3y = px, r3z = pw, r3w = pz;
            const float* c18 = c; const float* c19 = c + 4; const float* c20 = c + 8; const float* c21 = c + 12;
            float r2[4] = {r3w * c21[0], r3w * c21[3], r3w * c21[2], r3w * c21[1]};
            float t[4] = {r3z * c20[0] + r2[0], r3z * c20[3] + r2[1], r3z * c20[2] + r2[2], r3z * c20[1] + r2[3]};
            float u[4] = {t[0], t[2], t[3], t[1]};
            float r2n[4] = {r3y * c19[0] + u[0], r3y * c19[2] + u[1], r3y * c19[1] + u[2], r3y * c19[3] + u[3]};
            float o[4] = {r3x * c18[0] + r2n[0], r3x * c18[1] + r2n[2], r3x * c18[2] + r2n[1], r3x * c18[3] + r2n[3]};
            const float xn = o[0] * vi.ndc_scale[0] + vi.ndc_offset[0] * o[3], yn = o[1] * vi.ndc_scale[1] + vi.ndc_offset[1] * o[3];
            char s[160];
            std::snprintf(s, sizeof(s), " corner%u (%.0f,%.0f) uv(%g,%g) col %08X", corner,
                          (xn / o[3] * 0.5f + 0.5f) * vp.width + vp.x, (yn / o[3] * 0.5f + 0.5f) * vp.height + vp.y,
                          half(uint16_t(word(b + 4))), half(uint16_t(word(b + 4) >> 16)), word(b + 3));
            verts += s;
          }
        } else {
          for (uint32_t i = 0; i < std::min<uint32_t>(vb0.stride_words * 3, 15) && i < vf.size; i++) {
            char s[24];
            std::snprintf(s, sizeof(s), " %08X", word(i));
            verts += s;
          }
        }
      }
      // LEGO Batman port: the first 4 VS constant rows (a transform, if any) and the window offset.
      std::string consts;
      for (int k = 0; k < 16; k++) {
        char s[20];
        std::snprintf(s, sizeof(s), "%s%g", k % 4 ? "," : " c", regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + k));
        consts += s;
      }
      {
        auto wo = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
        char s[160];
        std::snprintf(s, sizeof(s), " winoff %d,%d vpz %g..%g ndc %g,%g,%g + %g,%g,%g", int(wo.window_x_offset), int(wo.window_y_offset), vp.near, vp.far,
                      vi.ndc_scale[0], vi.ndc_scale[1], vi.ndc_scale[2], vi.ndc_offset[0], vi.ndc_offset[1], vi.ndc_offset[2]);
        consts += s;
      }
      verts = consts + " |" + verts;
      char line[1700];
      std::snprintf(line, sizeof(line),
                    "#%u prim %u n=%u VS %08X PS %08X target %s%ux%u vp %g,%g %gx%g sc %d,%d %ux%u blend %u/%u/%u cull f%u b%u z %u/%u rects %d |%s | v:%s",
                    frame_draws_, uint32_t(prim_type), index_count, uint32_t(vs->ucode_data_hash()), uint32_t(ps->ucode_data_hash()),
                    color ? "" : "none ", color ? color->width : 0, color ? color->height : 0, vp.x, vp.y, vp.width, vp.height,
                    scissor.x, scissor.y, scissor.width, scissor.height,
                    uint32_t(regs.Get<reg::RB_BLENDCONTROL>().color_srcblend), uint32_t(regs.Get<reg::RB_BLENDCONTROL>().color_destblend),
                    uint32_t(regs.Get<reg::RB_BLENDCONTROL>().color_comb_fcn), su.cull_front, su.cull_back, dc.z_enable, dc.z_write_enable,
                    rects ? 1 : 0, bound_textures.c_str(), verts.c_str());
      trace_lines_.emplace_back(line);
    }
    if (ps->ucode_data_hash() == 0xB3EC630B32280C8Cull) frame_sprite_draws_++;
    draws_++;
    frame_draws_++;
#ifndef CVHD_DK_GL
    if (lbshot::want_draws == 2) {
      char d[160];
      std::snprintf(d, sizeof(d), "#%u VS %016llX PS %016llX prim %u n=%u", frame_draws_ - 1,
                    (unsigned long long)vs->ucode_data_hash(), (unsigned long long)ps->ucode_data_hash(), uint32_t(prim_type),
                    index_count);
      std::lock_guard<std::mutex> lk(lbshot::lock);
      lbshot::draws.push_back(d);
    }
#endif
    phase(6);
    return true;
  }

  bool IssueCopy() override {
    if (Skipping()) return true;
    SyncDirtyPages();
    return Resolve();
  }

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override {
    FlushResolveBarrier();
#ifdef CVHD_DK_GL
    // LEGO Batman port (testing): LB_SLOW_SWAP_MS=<n> makes the GPU thread n ms slower per frame, as on the console.
    static const int slow_ms = std::getenv("LB_SLOW_SWAP_MS") ? std::atoi(std::getenv("LB_SLOW_SWAP_MS")) : 0;
    if (slow_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(slow_ms));
#endif
    GpuMark(frames_[frame_], kTagPresent);
    PsMark(frames_[frame_], 4);
    StateCacheReset();  // the present binds its own shaders and states
    if (Skipping()) {
      swaps_++;  // nothing was recorded: the frame is just counted
      if (swaps_ % 60 == 0) {
        std::printf("[nx] gpu: swap #%u%s\n", swaps_, " (skipped)");  // the Eden gate's counter
        ReadFpsMode();
      }
      return;
    }
    Frame& f = frames_[frame_];
    DkCmdBuf cmd = f.cmdbuf;
    HostImage* front = nullptr;
    if (guest_images_.count(frontbuffer_ptr) && frontbuffer_width && frontbuffer_height) {
      // The resolve created it with the aligned height: the first swap trims it to the front buffer's size.
      front = GuestImage(f, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, DkImageFormat_RGBA8_Unorm, false);
      if (FrameLogOn()) Log("frm #%u swap front %08X img %05X", swaps_ + 1, frontbuffer_ptr, ImgId(front));
    }
    if (present_target_) {
      // Debug (the file sdmc:/switch/cvhd/dk_present_rt): show the widest colour render target itself,
      // bypassing the resolve copies and the front buffer image.
      HostImage* widest = nullptr;
      for (auto& it : targets_) if (!it.second->is_depth && (!widest || it.second->width > widest->width)) widest = it.second;
      if (widest) front = widest;
    }
    // Debug readback at two swaps: what the images hold (uniform colour = a broken stage).
    std::vector<std::pair<std::string, uint32_t>> dumps;
    const bool trace_frame = trace_enabled_ && !traced_ &&
        (trace_on_mip_ ? frame_mip_only_ : trace_min_swap_ == 0 ? (ring_full_swap_ && swaps_ + 1 >= ring_full_swap_ + trace_after_full_ && frame_big_tex_ && frame_draws_ > 250)
                              : (NowMs() - trace_start_ms_ >= trace_min_swap_ * 1000.0 && (trace_any_ || frame_draws_ > trace_min_draws_)));
    if (trace_frame) {
      traced_ = true;
      if (trace_second_) { traced_after_ = true; trace_min_swap_ = trace_second_; trace_second_ = 0; traced_ = false; }
      Log("trace of swap #%u: %zu draws", swaps_ + 1, trace_lines_.size());
      for (const std::string& l : trace_lines_) Log("  %s", l.c_str());
    }
    if (!trace_frame) trace_lines_.clear();
    // LEGO Batman port (flicker hunt): fingerprints of the front buffer, the colour targets and the big resolve images at
    // every swap of a 40-swap window: does the front buffer equal the target it was resolved from in the same frame?
    const bool fp_window = false;  // was swaps 600-639 (image fingerprints with GPU waits: a stutter in the intro)
    if (trace_frame || fp_window || (dump_images_ && (swaps_ + 1) % 300 == 0)) {
      auto queue_dump = [&](const char* what, HostImage* img) {
        if (!img) return;
        const bool is16 = img->format == DkImageFormat_BGR5A1_Unorm || img->format == DkImageFormat_R16_Unorm || img->format == DkImageFormat_BGR565_Unorm;
        uint32_t bytes = img->width * img->height * (is16 ? 2 : 4);
        uint32_t off = f.staging.Alloc(bytes, 0x1000);
        if (off == UINT32_MAX) return;
        DkImageView v;
        dkImageViewDefaults(&v, &img->image);
        DkImageRect r{0, 0, 0, img->width, img->height, 1};
        DkCopyBuf cb{f.staging.gpu + off, 0, 0};
        dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
        dkCmdBufCopyImageToBuffer(cmd, &v, &r, &cb, 0);
        char name[96];
        const char* tag = img->format == DkImageFormat_BGR5A1_Unorm ? " [1555]" : img->format == DkImageFormat_R16_Unorm ? " [r16]" : img->format == DkImageFormat_BGR565_Unorm ? " [565]" : "";
        std::snprintf(name, sizeof(name), "%s%s %ux%u", what, tag, img->width, img->height);
        dumps.emplace_back(name, off);
      };
      queue_dump("front", front);
      for (auto& it : targets_) if (!it.second->is_depth) { char n[48]; std::snprintf(n, sizeof(n), "target base %u pitch %u", it.second->edram_base, it.second->edram_pitch); queue_dump(n, it.second); }
      for (auto& it : guest_images_) if ((trace_frame || it.second->width >= 600) && it.second != front && (!fp_window || it.second->from_resolve)) { char n[48]; std::snprintf(n, sizeof(n), "guest %08X%s", it.first, it.second->from_resolve ? " (resolve)" : " (upload)"); queue_dump(n, it.second); }
      dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
    }
    if (swaps_ < 5) Log("swap #%u: acquiring a swapchain image", swaps_ + 1);
    const double acq0 = NowMs();
    int slot = dkQueueAcquireImage(queue_, swapchain_);
    t_acquire_ms_ += NowMs() - acq0;
    rex_nx_waited_add(uint64_t((NowMs() - acq0) * 19200.0));
    if (swaps_ < 5) Log("swap #%u: slot %d", swaps_ + 1, slot);
    HostImage* target = swap_images_[slot];
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    DkImageView tv;
    dkImageViewDefaults(&tv, &target->image);
    DkImageView const* targets[1] = {&tv};
    dkCmdBufBindRenderTargets(cmd, targets, 1, nullptr);
    DkViewport vp{0, 0, float(kPresentWidth), float(kPresentHeight), 0.0f, 1.0f};
    dkCmdBufSetViewports(cmd, 0, &vp, 1);
    DkScissor sc{0, 0, kPresentWidth, kPresentHeight};
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
    // Black (it was a debug navy: the user saw a blue flash at start, before the first front buffer exists).
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, black);
    if (front && present_vs_ && present_fs_) {
      DkRasterizerState rs;
      dkRasterizerStateDefaults(&rs);
      rs.cullMode = DkFace_None;
      dkCmdBufBindRasterizerState(cmd, &rs);
      DkColorState cs;
      dkColorStateDefaults(&cs);
      dkCmdBufBindColorState(cmd, &cs);
      DkColorWriteState cw;
      dkColorWriteStateDefaults(&cw);
      dkCmdBufBindColorWriteState(cmd, &cw);
      DkDepthStencilState ds;
      dkDepthStencilStateDefaults(&ds);
      ds.depthTestEnable = 0;
      ds.depthWriteEnable = 0;
      dkCmdBufBindDepthStencilState(cmd, &ds);
      DkShader const* shaders[2] = {&present_vs_shader_, &present_fs_shader_};
      dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, 2);
      dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
      dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
      // The overlay (present.frag "Hud"): the fps counter or the settings menu as a text grid from
      // nx_menu.cpp (size, grid, style, 16 row colours, two floats per cell). A full ring skips the
      // bind: one frame with a stale overlay is harmless.
      static NxOverlay ov;  // 6 KB: not on this thread's stack
      const int draw = rex_nx_menu_overlay(&ov, rex_nx_setting_fps_counter() ? hud_text_ : "", int(kPresentWidth), int(kPresentHeight));
      constexpr uint32_t kHudBytes = 3 * 16 + 16 * 16 + sizeof(ov.cells);
      uint32_t hud_off = f.data.Alloc(kHudBytes, DK_UNIFORM_BUF_ALIGNMENT);
      if (hud_off != UINT32_MAX) {
        float* u = reinterpret_cast<float*>(f.data.cpu + hud_off);
        u[0] = float(kPresentWidth); u[1] = float(kPresentHeight); u[2] = draw ? float(ov.cols) : 0.0f; u[3] = float(ov.rows);
        u[4] = float(ov.x); u[5] = float(ov.y); u[6] = float(6 * ov.scale); u[7] = float(8 * ov.scale);
        u[8] = float(ov.scale ? ov.scale : 1); u[9] = 0.85f; u[10] = ov.box ? 1.0f : 0.0f; u[11] = 1.0f;  // area average
        std::memcpy(u + 12, ov.row_rgb, sizeof(ov.row_rgb));
        std::memcpy(u + 12 + 64, ov.cells, sizeof(ov.cells));
        DkBufExtents hud_buf{f.data.gpu + hud_off, kHudBytes};
        dkCmdBufBindUniformBuffers(cmd, DkStage_Fragment, 0, &hud_buf, 1);
      }
      uint32_t dslot = DescriptorFor(f, front, 0);
      if (dslot != UINT32_MAX) {
        // The front buffer is 1920x1080 and the swapchain 1280x720 (x1.5). The sampler was index 4 = POINT (bit 4 is
        // the white border, not linear): every third row and column was dropped, so thin text strokes vanished and
        // fine detail shimmered on motion (0.141-0.265). Now linear with clamp-to-edge; present.frag averages four
        // linear taps over the output pixel's footprint (style.w). The DBG PRESENT toggle (AREA / LINEAR / POINT)
        // was removed for the release (0.280); git history before it has the A/B.
        const uint32_t clamp = (uint32_t(xenos::ClampMode::kClampToEdge) << 3) | (uint32_t(xenos::ClampMode::kClampToEdge) << 6);
        DkResHandle handle = dkMakeTextureHandle(dslot, clamp | 3u);
        dkCmdBufBindTextures(cmd, DkStage_Fragment, 0, &handle, 1);
        dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
      }
    }
    GpuMark(f, kTagDraw + 100);  // the end of the last segment (a tag no segment uses)
    PsMark(f, 0);                // the end of the last draw's segment
    if (f.ts_off != UINT32_MAX) dkCmdBufReportCounter(cmd, DkCounter_Timestamp, f.staging.gpu + f.ts_off + 16);
#ifndef CVHD_DK_GL
#ifndef CVHD_DK_GL
#ifndef LEGOBATMAN_RELEASE  // the L3+R3 recorder polls the pad every frame: development builds only
    {
      // LEGO Batman port: RECORD 5 SEC by clicking both sticks (L3+R3), on the swap after the press. cvhd starts it
      // from its port menu, which this port does not have. A PadState of our own (the game's input driver reads
      // its own; libnx pads read the shared HID memory independently).
      static PadState rec_pad;
      static bool rec_pad_init = false;
      static bool rec_was_down = false;
      if (!rec_pad_init) { padInitializeDefault(&rec_pad); rec_pad_init = true; }
      padUpdate(&rec_pad);
      const u64 held = padGetButtons(&rec_pad);
      const bool down = (held & (HidNpadButton_StickL | HidNpadButton_StickR)) == (HidNpadButton_StickL | HidNpadButton_StickR);
      if (down && !rec_was_down && !rex_nx_debug_record()) {
        Log("RECORD 5 SEC: L3+R3 at swap #%u", swaps_ + 1);
        rex_nx_debug_record_set(1);
      }
      rec_was_down = down;
    }
#endif
#endif
    RecordFrame(f, cmd, front);
    uint32_t shot_off = UINT32_MAX, shot_w = 0, shot_h = 0;
    {
      std::lock_guard<std::mutex> lk(lbshot::lock);
      if (lbshot::want_draws == 2) { lbshot::want_draws = 0; lbshot::draws_ready = true; lbshot::cv.notify_all(); }
      else if (lbshot::want_draws == 1) lbshot::want_draws = 2;
      if (lbshot::requested && front && front->format == DkImageFormat_RGBA8_Unorm) {
        shot_w = front->width;
        shot_h = front->height;
        shot_off = f.staging.Alloc(shot_w * shot_h * 4, 0x1000);
        if (shot_off != UINT32_MAX) {
          DkImageView v;
          dkImageViewDefaults(&v, &front->image);
          DkImageRect r{0, 0, 0, shot_w, shot_h, 1};
          DkCopyBuf cb{f.staging.gpu + shot_off, 0, 0};
          dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
          dkCmdBufCopyImageToBuffer(cmd, &v, &r, &cb, 0);
          lbshot::requested = false;
        }
      }
    }
#endif
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
    DkCmdList list = dkCmdBufFinishList(cmd);
    dkQueueSubmitCommands(queue_, list);
    dkQueueSignalFence(queue_, &f.fence, false);
    f.fenced = true;
    dkQueuePresentImage(queue_, swapchain_, slot);
#ifndef CVHD_DK_GL
    // LEGO Batman port: the console overlays (Status Monitor Overlay through SaltyNX, SDK switch_saltynx.cpp) need a
    // heartbeat on every present and read FPS and RES from it; only the SDK's Vulkan renderer sent it, so the
    // overlay said "Game is not running" and the profiler counted 0 game fps. RES is the 3D render resolution.
    RexSwitchPerfResolution(1280 * hs_ / kScaleDen, 720 * hs_ / kScaleDen);
    RexSwitchPerfCount(0);
#endif
#ifndef CVHD_DK_GL
    // LEGO Batman port (flicker test, 2026-11-10): finish each frame on the GPU before the CPU records the next, as the
    // GL host does. If the console's alternating-frame flicker disappears, it is a hazard between overlapping frames.
    // Remove once the hazard is found (it costs the CPU/GPU overlap).
    if (shot_off != UINT32_MAX) {
      dkQueueWaitIdle(queue_);  // one stall per screenshot
      std::lock_guard<std::mutex> lk(lbshot::lock);
      lbshot::px.assign(f.staging.cpu + shot_off, f.staging.cpu + shot_off + size_t(shot_w) * shot_h * 4);
      lbshot::w = shot_w;
      lbshot::h = shot_h;
      lbshot::ready = true;
      lbshot::cv.notify_all();
    }
#endif
    // Shown-frame intervals: what the screen gets (the [nx] fps line counts guest swaps).
    const double shown_now = NowMs();
    if (shown_last_ms_) shown_ms_.push_back(shown_now - shown_last_ms_);
    shown_last_ms_ = shown_now;
#ifdef CVHD_DK_GL
    FlushTextureDumps();
#endif
    if (!dumps.empty()) {
      dkQueueWaitIdle(queue_);
      for (auto& d : dumps) {
        // The image's dims are in the name; recover them from the staging layout: sample a 16x16 grid.
        uint32_t w = 0, h = 0;
        std::sscanf(d.first.c_str() + d.first.rfind(' ') + 1, "%ux%u", &w, &h);
        const uint32_t* px = reinterpret_cast<const uint32_t*>(f.staging.cpu + d.second);
        std::unordered_set<uint32_t> distinct;
        for (uint32_t y = 0; y < 16 && h; y++)
          for (uint32_t x = 0; x < 16 && w; x++) distinct.insert(px[(y * h / 16) * w + x * w / 16]);
        if (fp_window && !trace_frame) {
          // Fingerprint of the top 720 rows (the visible area) of the image.
          uint64_t fh = 1469598103934665603ull;
          const uint32_t rows = std::min<uint32_t>(h, 720), stride = 7;
          for (uint32_t y = 0; y < rows; y += 3)
            for (uint32_t x = 0; x < w; x += stride) fh = (fh ^ px[size_t(y) * w + x]) * 1099511628211ull;
          Log("fp swap #%u %s: %016llX centre %08X", swaps_ + 1, d.first.c_str(), (unsigned long long)fh,
              w && h ? px[(std::min<uint32_t>(h, 720) / 2) * w + w / 2] : 0);
          continue;
        }
        Log("dump at swap #%u: %s: %zu distinct colours in a 16x16 grid; pixels %08X %08X %08X %08X %08X", swaps_ + 1,
            d.first.c_str(), distinct.size(), w && h ? px[0] : 0, w && h ? px[(h / 4) * w + w / 4] : 0,
            w && h ? px[(h / 2) * w + w / 2] : 0, w && h ? px[(3 * h / 4) * w + 3 * w / 4] : 0, w && h ? px[(h - 1) * w + w - 1] : 0);
        if (trace_frame && w && h) {
          // The traced frame's images as PPM files next to the shader directory: dump/<n>.ppm (names in the log).
          static uint32_t dump_index = 0;
          const std::string dir = debug_dir_ + "dump";
          mkdir(dir.c_str(), 0777);
          char path[160];
          std::snprintf(path, sizeof(path), "%s/%02u.ppm", dir.c_str(), dump_index);
          Log("  image file %s = %s", path, d.first.c_str());
          dump_index++;
          if (FILE* pf = std::fopen(path, "wb")) {
            std::fprintf(pf, "P6\n%u %u\n255\n", w, h);
            std::vector<uint8_t> row(w * 3);
            for (uint32_t y = 0; y < h; y++) {
              for (uint32_t x = 0; x < w; x++) {
                uint32_t r, g, b;
                if (d.first.find("[1555]") != std::string::npos) {  // A1 R5 G5 B5 (Xenos 1_5_5_5)
                  const uint16_t p16 = reinterpret_cast<const uint16_t*>(px)[y * w + x];
                  r = ((p16 >> 10) & 31) * 255 / 31; g = ((p16 >> 5) & 31) * 255 / 31; b = (p16 & 31) * 255 / 31;
                } else if (d.first.find("[565]") != std::string::npos) {
                  const uint16_t p16 = reinterpret_cast<const uint16_t*>(px)[y * w + x];
                  r = ((p16 >> 11) & 31) * 255 / 31; g = ((p16 >> 5) & 63) * 255 / 63; b = (p16 & 31) * 255 / 31;
                } else if (d.first.find("[r16]") != std::string::npos) {  // index texture: the high byte as grey, the low byte in blue
                  const uint16_t p16 = reinterpret_cast<const uint16_t*>(px)[y * w + x];
                  r = g = p16 >> 8; b = p16 & 255;
                } else {
                  const uint32_t p = px[y * w + x];  // RGBA8 bytes R,G,B,A in memory order
                  r = p & 255; g = (p >> 8) & 255; b = (p >> 16) & 255;
                }
                row[x * 3 + 0] = uint8_t(r);
                row[x * 3 + 1] = uint8_t(g);
                row[x * 3 + 2] = uint8_t(b);
              }
              std::fwrite(row.data(), 1, row.size(), pf);
            }
            std::fclose(pf);
          }
        }
      }
    }
    swaps_++;
    rex_nx_swaps = swaps_;
#ifndef CVHD_DK_GL
    if (int n = lbshot::flags[11].exchange(0); n > 0) frame_log_until_ = swaps_ + 1 + uint32_t(n);
#endif
    if (swaps_ % 60 == 0) ReadFpsMode();
    // The fps counter: swaps per second over a one-second window, drawn by the next present passes.
    const auto now = std::chrono::steady_clock::now();
    if (hud_window_start_ == std::chrono::steady_clock::time_point{}) hud_window_start_ = now;
    hud_window_swaps_++;
    const double elapsed = std::chrono::duration<double>(now - hud_window_start_).count();
    if (elapsed >= 1.0) {
      // fps, then the main thread's busy ms per frame (switch_nx.cpp's busy line): "36 9.8".
      std::snprintf(hud_text_, sizeof(hud_text_), "%.0f %.1f", hud_window_swaps_ / elapsed, rex_nx_main_busy_ms);
      std::sort(shown_ms_.begin(), shown_ms_.end());
      const size_t sn = shown_ms_.size();
      const size_t le34 = std::upper_bound(shown_ms_.begin(), shown_ms_.end(), 34.0) - shown_ms_.begin();
      std::printf("[nx] dk time/s: draws+resolves %.0f ms, acquire %.0f ms, fence wait %.0f ms, gpu %.0f ms/%u frames, over %u frames (%.0f ms window); "
                  "draw phases ms: sync %.0f setup %.0f tex %.0f state %.0f vtx %.0f uni %.0f draw %.0f; uniform uploads vs %u ps %u sys %u; texture bind hits %u; late writes %u; "
                  "shown %zu: interval p50 %.1f p95 %.1f ms, %zu%% <= 34 ms\n",
                  t_draw_ms_, t_acquire_ms_, t_fence_ms_, t_gpu_ms_, gpu_frames_, hud_window_swaps_, elapsed * 1000.0, t_phase_ms_[0], t_phase_ms_[1],
                  t_phase_ms_[2], t_phase_ms_[3], t_phase_ms_[4], t_phase_ms_[5], t_phase_ms_[6], uni_uploads_[0], uni_uploads_[1],
                  uni_uploads_[2], texbind_hits_, late_writes_, sn, sn ? shown_ms_[sn / 2] : 0.0, sn ? shown_ms_[sn * 95 / 100] : 0.0, sn ? le34 * 100 / sn : 0);
      shown_ms_.clear();
      texbind_hits_ = late_writes_ = 0;
      std::printf("[nx] dk persist/s: %u hits, %u copies, %u became dynamic; pool %u/%u MB, %u resets%s\n", persist_hits_, persist_copies_,
                  persist_dynamic_, persist_.used >> 20, persist_.size >> 20, persist_resets_, PersistOn() ? "" : " (off)");
      persist_hits_ = persist_copies_ = persist_dynamic_ = 0;
      std::printf("[nx] dk render scale %u/12 (%ux%u), GPU %.1f ms/frame smoothed; pools: scaled %u/%u MB, textures %u/%u MB\n", hs_,
                  1280 * hs_ / kScaleDen, 720 * hs_ / kScaleDen, gpu_ema_, scaled_images_.used >> 20, scaled_images_.size >> 20, images_.used >> 20,
                  images_.size >> 20);
      std::printf("[nx] dk resolves/s: %u repeated copies skipped; constants clamped/s: %u at write, %u still extreme at upload (e.g. c%u of a block)\n",
                  resolves_skipped_, const_clamped_, const_late_, const_late_reg_);
      const_clamped_ = const_late_ = 0;
      const_late_reg_ = ~0u;
      if (ps_cost_frames_) {
        std::vector<std::pair<uint64_t, PsCost>> v(ps_cost_.begin(), ps_cost_.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
        double total = 0;
        for (auto& e : v) total += e.second.ms;
        std::printf("[nx] dk ps cost over %u frames: total %.1f ms/frame;", ps_cost_frames_, total / ps_cost_frames_);
        for (size_t i = 0; i < v.size() && i < 16; i++)
          std::printf(" %016llX %.2f ms/%.0f draws;", (unsigned long long)v[i].first, v[i].second.ms / ps_cost_frames_,
                      double(v[i].second.draws) / ps_cost_frames_);
        std::printf("\n");
        ps_cost_.clear();
        ps_cost_frames_ = 0;
      }
      resolves_skipped_ = 0;
      if (t_cat_marks_) {
        // flag 6: GPU ms per second in each category (see GpuMark)
        std::printf("[nx] dk gpu by category ms/s: draws %.0f clears %.0f resolves %.0f uploads %.0f target-switch %.0f present %.0f (%u marks)\n",
                    t_cat_ms_[kTagDraw], t_cat_ms_[kTagClear], t_cat_ms_[kTagResolve], t_cat_ms_[kTagUpload], t_cat_ms_[kTagTarget],
                    t_cat_ms_[kTagPresent], t_cat_marks_);
        for (double& v : t_cat_ms_) v = 0;
        t_cat_marks_ = 0;
      }
      t_draw_ms_ = t_acquire_ms_ = t_fence_ms_ = t_gpu_ms_ = 0;
      gpu_frames_ = 0;
      for (double& t : t_phase_ms_) t = 0;
      for (uint32_t& u : uni_uploads_) u = 0;
      hud_window_start_ = now;
      hud_window_swaps_ = 0;
    }
    // Textures uploaded in this frame whose guest memory was written again before the swap: the game rewrote a sprite
    // slot after our snapshot while the draw that samples it is still queued (the blink hypothesis REUPLOAD = ALL
    // cannot exclude). The marks of those late writes are consumed here; the next frame re-uploads them anyway.
    SyncDirtyPages();
    for (const Uploaded& u : frame_uploaded_) if (WrittenSince(u.base, u.bytes, u.serial)) late_writes_++;
    frame_uploaded_.clear();
    if (swaps_ <= 3 || swaps_ % 60 == 0) {
      std::printf("[nx] gpu: swap #%u\n", swaps_);  // the Eden gate's frame counter
      Log("swap #%u: front %08X %ux%u %s; frame: %u draws, %u clears, %u resolves, %u skipped, %u no-shader, %u re-uploads, %llu dirty pages; data %u KB staging %u KB; images %u (%u MB)",
          swaps_, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, front ? "shown" : "MISSING", frame_draws_,
          frame_clears_, frame_resolves_, frame_skipped_, frame_missing_, frame_reuploads_, (unsigned long long)(dirty_pages_ - dirty_pages_at_swap_),
          f.data.used >> 10, f.staging.used >> 10, image_count_, images_.used >> 20);
      if (frame_skipped_)
        Log("swap #%u: skipped by reason: no shader object %u, writes nothing %u, primitive %u, texture %u, no target %u, indices %u, "
            "data ring %u, uniforms %u", swaps_, frame_skip_why_[0], frame_skip_why_[1], frame_skip_why_[2], frame_skip_why_[3],
            frame_skip_why_[4], frame_skip_why_[5], frame_skip_why_[6], frame_skip_why_[7]);
    }
    if (swaps_ % 60 == 0 && (frame_biased_ || frame_stenciled_))
      Log("swap #%u: %u draws with depth bias, %u with stencil", swaps_, frame_biased_, frame_stenciled_);
    if (swaps_ % 60 == 0 && frame_tile_dups_)
      Log("swap #%u: single-pass tiles: %u bottom-tile draws skipped as duplicates, %u kept per tile (tile-resolve readers)", swaps_,
          frame_tile_dups_, frame_tile_dependent_);
    frame_tile_dups_ = 0;
    frame_tile_dependent_ = 0;
    cur_tile_off_ = 0;
    tile_post_ = false;
    tile_keys_.clear();
    if (frame_tile_h_) tile_h_ = frame_tile_h_;
    frame_tile_h_ = 0;
    prev_frame_tiled_ = frame_offset_draws_;
    frame_offset_draws_ = false;
    frame_biased_ = frame_stenciled_ = 0;
    for (uint32_t& c : frame_skip_why_) c = 0;
    dirty_pages_at_swap_ = dirty_pages_;
    frame_draws_ = frame_clears_ = frame_resolves_ = frame_skipped_ = frame_missing_ = frame_reuploads_ = 0;
    frame_sprite_draws_ = 0;
    frame_big_tex_ = false;
    frame_mip_only_ = false;
    frame_ = (frame_ + 1) % kFrames;
    BeginFrame();
  }

 private:
  // The frame-rate mode, file sdmc:/switch/cvhd/fps (next to the shader dir), read at start and every 60 swaps, so it
  // switches during a run (the user's A/B needs no build): no file or "60" = every frame drawn, presented at the next
  // vblank; "30" = every frame drawn, presented every second vblank (the swapchain acquire paces the guest at 30 Hz);
  // The game is time-based (Eden 0.237: 96-99% of real time at 30 and 60), so "30" is the even mode. The "30skip" mode
  // (every second frame not drawn) was removed on 2026-11-05: on the console it showed ~20 fps (user's report).
  void ReadFpsMode() {
    if (!debug_dir_.empty() && unlink((debug_dir_ + "record_now").c_str()) == 0) rex_nx_debug_record_set(1);
    // The settings menu (nx_menu.cpp, settings.txt; the old marker file sdmc:/switch/cvhd/fps is imported
    // once when there is no settings file yet).
    const int mode = rex_nx_setting_fps_mode();
    const uint32_t interval = mode == 1 ? 2 : 1;
    // Mode 30 also halves the guest's vblank interrupt rate (the SDK's vsync worker, sdk-edits rule 31): the game
    // starts its frames at a vblank and times them by mftb, so with 60 Hz vblanks under a 30 Hz present its frames
    // alternated 17 / 50 ms (console 0.249: 5-8 frames of 50 ms per second with the presents even) = uneven
    // motion. The file sdmc:/switch/cvhd/vblank60 keeps the 60 Hz vblanks (the 0.249 behaviour) for an A/B.
    const bool vblank60 = access((debug_dir_ + "vblank60").c_str(), F_OK) == 0;
    const int divider = interval == 2 && !vblank60 ? 2 : 1;
    if (interval == swap_interval_ && divider == rex_nx_vblank_divider && swaps_) return;
    swap_interval_ = interval;
    rex_nx_vblank_divider = divider;
#ifndef CVHD_DK_GL
    dkSwapchainSetSwapInterval(swapchain_, interval);
#endif
    std::printf("[nx] gpu: fps mode %s: swap interval %u, vblank interrupts at %u Hz from swap #%u\n",
                interval == 2 ? "30" : "60", interval, 60u / divider, swaps_);
    std::fflush(stdout);
  }

  // Linux test host: CVHD_DK_START_SWAP=N turns the backend into a null backend until swap N (the guest runs
  // at full speed to a late scene; textures and targets are created lazily when drawing starts).
  // The file <CVHD_DK_DUMP_DIR>/skip_on does the same while it exists (checked every 8 swaps): fast forward
  // through loading screens, then delete it.
  bool Skipping() const {
#ifdef CVHD_DK_GL
    static const uint32_t start = std::getenv("CVHD_DK_START_SWAP") ? uint32_t(std::atoi(std::getenv("CVHD_DK_START_SWAP"))) : 0;
    static const char* dir = std::getenv("CVHD_DK_DUMP_DIR");
    static bool file_skip = false;
    if (dir && (swaps_ & 7) == 0 && !skip_checked_[0]) {
      file_skip = access((std::string(dir) + "/skip_on").c_str(), F_OK) == 0;
      skip_checked_[0] = true;
    } else if ((swaps_ & 7) != 0) {
      skip_checked_[0] = false;
    }
    return file_skip || swaps_ + 1 < start;
#else
    return false;
#endif
  }
  mutable bool skip_checked_[1] = {false};

  // ----- frames ---------------------------------------------------------------------------------
  void BeginFrame() {
    Frame& f = frames_[frame_];
    if (f.fenced) {
      if (swaps_ < 6) Log("frame %u: waiting for its fence", frame_);
      const double fw0 = NowMs();
      DkResult r = dkFenceWait(&f.fence, 2000000000ll);  // 2 s: a hang is logged, not silent
      t_fence_ms_ += NowMs() - fw0;
      rex_nx_waited_add(uint64_t((NowMs() - fw0) * 19200.0));
      if (r != DkResult_Success) Log("frame %u: fence wait result %d (GPU hang?)", frame_, int(r));
      if (swaps_ < 6) Log("frame %u: fence done", frame_);
      f.fenced = false;
    }
#ifndef CVHD_DK_GL
    if (f.rec_pending) {
      const uint32_t bytes = kRecW * kRecH * 4;
      dkMemBlockFlushCpuCache(rec_.block, frame_ * bytes, bytes);
      rex_rec_push(rec_.cpu + frame_ * bytes, f.rec_swap, f.rec_us);
      f.rec_pending = false;
    }
#endif
    if (f.ts_off != UINT32_MAX) {
      const uint64_t* r = reinterpret_cast<const uint64_t*>(f.staging.cpu + f.ts_off);
      const uint64_t t0 = r[1] ? r[1] : r[0], t1 = r[3] ? r[3] : r[2];
      if (gpu_frames_ < 3) Log("gpu timestamps: top %llu/%llu bottom %llu/%llu", (unsigned long long)r[0], (unsigned long long)r[1],
                               (unsigned long long)r[2], (unsigned long long)r[3]);
      if (t1 > t0) {
        const double ms = double(dkTimestampToNs(t1 - t0)) / 1e6;
        t_gpu_ms_ += ms;
        gpu_frames_++;
        gpu_ema_ = gpu_ema_ > 0 ? gpu_ema_ * 0.9 + ms * 0.1 : ms;  // the render scale's input
      }
      f.ts_off = UINT32_MAX;
    }
    if (f.mk_off != UINT32_MAX) {
      const uint64_t* r = reinterpret_cast<const uint64_t*>(f.staging.cpu + f.mk_off);
      for (uint32_t i = 0; i + 1 < f.mk_n; i++) {
        const uint64_t a = r[2 * i + 1] ? r[2 * i + 1] : r[2 * i], b = r[2 * i + 3] ? r[2 * i + 3] : r[2 * i + 2];
        if (b > a && f.mk_tag[i] < kTagCount) t_cat_ms_[f.mk_tag[i]] += double(dkTimestampToNs(b - a)) / 1e6;
      }
      t_cat_marks_ += f.mk_n;
      f.mk_off = UINT32_MAX;
      f.mk_n = 0;
    }
    if (f.pm_off != UINT32_MAX) {
      const uint64_t* r = reinterpret_cast<const uint64_t*>(f.staging.cpu + f.pm_off);
      for (uint32_t i = 0; i + 1 < f.pm_n; i++) {
        const uint64_t a = r[2 * i + 1] ? r[2 * i + 1] : r[2 * i], b = r[2 * i + 3] ? r[2 * i + 3] : r[2 * i + 2];
        if (b <= a || f.pm_hash[i] == 0) continue;
        PsCost& c = ps_cost_[f.pm_hash[i]];
        c.ms += double(dkTimestampToNs(b - a)) / 1e6;
        c.draws++;
      }
      ps_cost_frames_++;
      f.pm_off = UINT32_MAX;
      f.pm_n = 0;
    }
    RestampRecentPages();
    f.data.Reset();
    f.staging.Reset();
    PersistFrameStart();
    StateCacheReset();
    ApplyRenderScale();
    f.next_descriptor = 0;
    f.descriptors.clear();
    f.texbinds.clear();
    f.copied.clear();
    dkCmdBufClear(f.cmdbuf);
    dkCmdBufAddMemory(f.cmdbuf, cmdmem_.block, f.cmd_offset, kFrameCmdBytes);
    f.ts_off = f.staging.Alloc(32, 16);
#ifndef CVHD_DK_GL
    if (lbshot::flags[6]) {
      f.mk_off = f.staging.Alloc(Frame::kMaxMarks * 16, 16);
      f.mk_n = 0;
      if (f.mk_off != UINT32_MAX) std::memset(f.staging.cpu + f.mk_off, 0, Frame::kMaxMarks * 16);
      GpuMark(f, kTagDraw);
    }
    if (lbshot::flags[14]) {
      f.pm_off = f.staging.Alloc(Frame::kMaxPsMarks * 16, 16);
      f.pm_n = 0;
      if (f.pm_off != UINT32_MAX) std::memset(f.staging.cpu + f.pm_off, 0, Frame::kMaxPsMarks * 16);
      PsMark(f, 3);
    }
#endif
    if (f.ts_off != UINT32_MAX) {
      std::memset(f.staging.cpu + f.ts_off, 0, 32);
      dkCmdBufReportCounter(f.cmdbuf, DkCounter_TimestampPipelineTop, f.staging.gpu + f.ts_off);
    }
    // LEGO Batman port: this frame's ring (uniforms, vertices, indices), staging and descriptors reuse the addresses of
    // the frame two swaps ago, rewritten by the CPU during recording. GpuUncached memory keeps them out of L2, but the
    // GPU's shader data/uniform cache (and the descriptor cache) can still hold the old lines: one parity of frames
    // drew with constants from two frames back (console only: the WB logo scene alternated purple/green in the frame
    // recorder, 2026-11-10; the GL host executes synchronously). The whole frame is submitted once at the swap, so one
    // invalidation at the start of its command buffer covers every CPU write of the frame.
    dkCmdBufBarrier(f.cmdbuf, DkBarrier_None,
                    DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors | DkInvalidateFlags_L2Cache);
    dkCmdBufBindImageDescriptorSet(f.cmdbuf, descriptors_.gpu + f.desc_offset, kMaxImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(f.cmdbuf, descriptors_.gpu + sampler_desc_offset_, kSamplerCount);
    bound_color_ = nullptr;
    bound_depth_ = nullptr;
    f.vs_uni = f.ps_uni = f.sys_uni = UINT32_MAX;
    f.vs_uni_shader = f.ps_uni_shader = nullptr;
    f.vfetch_shader = nullptr;
  }

  // Every register write of the command stream (SET_CONSTANT, LOAD_ALU_CONSTANT, REG_RMW ...) comes through here:
  // the float constant ranges mark their uniform block stale.
  void WriteRegister(uint32_t index, uint32_t value) override {
    CommandProcessor::WriteRegister(index, value);
    if (index < XE_GPU_REG_SHADER_CONSTANT_000_X) return;
    if (index <= XE_GPU_REG_SHADER_CONSTANT_511_W && SanitizeOn())
      const_clamped_ += SanitizeFloats(&register_file_->values[index], 1);  // finite, |x| <= 2^60 (SanitizeFloats)
    if (index < XE_GPU_REG_SHADER_CONSTANT_256_X) vs_consts_dirty_ = true;
    else if (index <= XE_GPU_REG_SHADER_CONSTANT_511_W) ps_consts_dirty_ = true;
  }
  // SET_CONSTANT / LOAD_ALU_CONSTANT write the shader constants (float 0x4000-0x47FF, fetch 0x4800-0x48FF, bool and
  // loop 0x4900-0x4927) a dword at a time through the virtual WriteRegister (a bounds check, a register-info lookup,
  // the scratch/COHER/LUT switch each): the game writes constants before nearly every draw, ~80,000 dwords a frame
  // (Eden 0.218: the command thread's non-draw work was ~6 ms per frame). None of those registers is special, so a
  // range inside them is copied straight into the register file (byte-swapped) with one dirty mark.
  void WriteRegistersFromMem(uint32_t start_index, uint32_t* base, uint32_t num_registers) override {
    const uint32_t end = start_index + num_registers;
    if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X && end <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1 && num_registers) {
      uint32_t* dst = &register_file_->values[start_index];
      for (uint32_t i = 0; i < num_registers; ++i) dst[i] = rex::memory::load_and_swap<uint32_t>(base + i);
      // The float constants made finite and at most 2^60 once, as written (SanitizeFloats): each upload re-checking
      // them cost ~10 ms of CPU per frame.
      if (start_index <= XE_GPU_REG_SHADER_CONSTANT_511_W && SanitizeOn()) {
        const uint32_t fend = std::min(end, uint32_t(XE_GPU_REG_SHADER_CONSTANT_511_W + 1));
        const_clamped_ += SanitizeFloats(dst, fend - start_index);
      }
      if (start_index < XE_GPU_REG_SHADER_CONSTANT_256_X) vs_consts_dirty_ = true;
      if (end > XE_GPU_REG_SHADER_CONSTANT_256_X && start_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) ps_consts_dirty_ = true;
      return;
    }
    CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
  }

  // ----- dirty pages ----------------------------------------------------------------------------
  // Consumes the dirty-page bytes before every draw and stamps each written page with the draw
  // serial; a texture whose pages were written after the upload or resolve that filled its image
  // is uploaded again at its next bind (TextureImage). The coarse map makes a clean frame cost 512
  // byte reads. ponytail: the guest marks a page after the store without a barrier, so a texture
  // written in the same microsecond as its bind can be uploaded stale once (the next write fixes it).
  void SyncDirtyPages() {
    serial_++;
#ifdef LEGOBATMAN_HASH_TEXTURES
    return;  // LEGO Batman port: no dirty pages in hash mode (the scan cost ~60 ms/s)
#endif
    // The guest threads mark a page after its store; on the Tegra (ARM) the dirty byte and the data can be seen out of
    // order without a fence: the data read after this point is at least as new as the marks read here.
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const uint64_t* words = reinterpret_cast<const uint64_t*>(const_cast<const uint8_t*>(rex_nx_dirty));
    for (uint32_t c = 0; c < 0x200; c++) {
      // A region seen dirty in this or the previous frame is scanned again even when its coarse byte is 0: on ARM the
      // writer's coarse byte can become visible before its fine byte, and clearing the coarse byte then lost the fine
      // one (a texture that updated seconds late or never: the grey portrait, the green flashes; 0.266).
      if (!rex_nx_dirty_coarse[c] && !recent_coarse_[c]) continue;
      if (rex_nx_dirty_coarse[c]) recent_coarse_[c] = 2;
      rex_nx_dirty_coarse[c] = 0;
      for (uint32_t w = c * 32; w < c * 32 + 32; w++) {
        if (!words[w]) continue;
        for (uint32_t i = w * 8; i < w * 8 + 8; i++) {
          if (!rex_nx_dirty[i]) continue;
          rex_nx_dirty[i] = 0;
          page_serial_[i] = serial_;
          if (!page_seen_[i]) { page_seen_[i] = 1; frame_pages_.push_back(i); }
          dirty_pages_++;
        }
      }
    }
  }
  // At a frame start: the pages written in the previous frame are stamped again. A vector store (stvx, dcbz, stwcx)
  // marks its page BEFORE the store (REX_RAW_STORE_ADDR), so an upload between the mark and the store read the old
  // bytes and the image looked current from then on. Re-stamping makes the next bind compare again with landed data.
  void RestampRecentPages() {
    serial_++;
    for (uint32_t p : last_frame_pages_) page_serial_[p] = serial_;
    for (uint32_t p : frame_pages_) page_seen_[p] = 0;
    last_frame_pages_.swap(frame_pages_);
    frame_pages_.clear();
    for (uint32_t c = 0; c < 0x200; c++) if (recent_coarse_[c]) recent_coarse_[c]--;
    frame_start_serial_ = serial_;
  }
  bool WrittenSince(uint32_t physical, uint32_t bytes, uint32_t serial) const {
    uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
    uint32_t last = ((physical & 0x1FFFFFFFu) + (bytes ? bytes - 1 : 0)) >> 12;
    for (uint32_t p = first; p <= last && p < page_serial_.size(); p++) {
      if (page_serial_[p] > serial) return true;
    }
    return false;
  }

  // Whether the guest memory behind an image changed since its contents were set. Switch: the dirty-page
  // serials. Linux: a hash of the range against the hash taken when the contents were set.
  uint64_t HashMemory(uint32_t physical, uint32_t bytes, bool full = false) const {
#if defined(CVHD_DK_GL) || defined(LEGOBATMAN_HASH_TEXTURES)
    const uint8_t* p = memory_->TranslatePhysical(physical);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; h ^= h >> 29; };
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    if (full || bytes <= 8192) {  // small images and dynamic ones: every word
      for (uint32_t i = 0; i < bytes / 8; i++) mix(w[i]);
      return h;
    }
    // Large images: four words per 4 KB page at offsets that differ per page (a texture load or a video frame
    // rewrites the whole image; a partial write that misses every sample is the accepted risk).
    for (uint32_t page = 0; page + 4096 <= bytes; page += 4096) {
      const uint32_t base = page / 8, k = (page >> 12) & 15;
      mix(w[base + k]); mix(w[base + 128 + k]); mix(w[base + 256 + k]); mix(w[base + 384 + 2 * k]);
    }
    mix(w[bytes / 8 - 1]);
    return h;
#else
    (void)physical; (void)bytes;
    return 0;
#endif
  }
  bool MemoryChanged(const HostImage& img, uint32_t physical, uint32_t bytes) const {
#if defined(CVHD_DK_GL) || defined(LEGOBATMAN_HASH_TEXTURES)
    (void)bytes;
    // LEGO Batman port: a resolve destination is never refreshed from guest memory. The backend does not write
    // resolves back, so the memory holds stale bytes; re-uploading them replaced the rendered scene with garbage
    // for a frame (the console flicker). The game writing a resolve target from the CPU is not handled.
    if (img.from_resolve) return false;
#ifndef CVHD_DK_GL
    if (lbshot::flags[0] && img.full_hash) return true;  // debug server "flag 0 1": always re-upload dynamic textures
#endif
    return HashMemory(physical, img.mem_bytes, img.full_hash) != img.mem_hash;
#else
    return WrittenSince(physical, bytes, img.content_serial);
#endif
  }

  // ----- images ---------------------------------------------------------------------------------
  // gh: images replaced by a resize (or a format change) wait here for a request of the same shape: the
  // pool is a bump allocator and a per-frame resize ping-pong filled 448 MB in minutes (finding 0.22).
  std::vector<HostImage*> free_images_;
  void RecycleImage(HostImage* img) {
    if (!img) return;
    if (img->scaled_pool) { retired_scaled_.push_back(img); return; }  // freed with the pool at the next reset
    if (!img->is_depth) free_images_.push_back(img);
  }

  HostImage* CreateImage(uint32_t width, uint32_t height, DkImageFormat format, uint32_t flags, bool is_depth,
                         bool scaled_pool = false) {
    for (size_t i = 0; !scaled_pool && i < free_images_.size(); i++) {
      HostImage* r = free_images_[i];
      if (r->width == width && r->height == height && r->format == format && r->flags == flags && r->is_depth == is_depth) {
        free_images_.erase(free_images_.begin() + i);
        r->from_resolve = false;
        r->content_serial = 0;
        r->checked_serial = 0;
        r->checked_frame = ~0u;
        r->full_hash = false;
        r->guest_base = 0;
        r->edram_base = r->edram_pitch = 0;
        r->gw = width;
        r->gh = height;
        r->hs = kScaleDen;
        return r;
      }
    }
    auto* img = new HostImage();
    img->flags = flags;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device_);
    lm.flags = flags;
    lm.format = format;
    lm.dimensions[0] = width;
    lm.dimensions[1] = height;
    lm.dimensions[2] = 1;
    dkImageLayoutInitialize(&img->layout, &lm);
    uint64_t size = dkImageLayoutGetSize(&img->layout);
    uint32_t align = dkImageLayoutGetAlignment(&img->layout);
    MemBlock& pool = scaled_pool ? scaled_images_ : images_;
    uint32_t off = pool.Alloc(uint32_t(size), align);
    if (off == UINT32_MAX) {
      // Rate-limited: one line per pool full a second (a line per draw to the SD card stalled the game).
      if (swaps_ != pool_full_logged_) {
        pool_full_logged_ = swaps_;
        Log("%s image pool full (%u MB used): %ux%u not created", scaled_pool ? "scaled" : "texture", pool.used >> 20, width, height);
      }
      if (scaled_pool) scaled_full_ = true;  // reset at the next frame start
      delete img;
      return nullptr;
    }
    dkImageInitialize(&img->image, &img->layout, pool.block, off);
    img->scaled_pool = scaled_pool;
    img->format = format;
    img->width = width;
    img->height = height;
    img->gw = width;
    img->gh = height;
    img->hs = kScaleDen;
    img->is_depth = is_depth;
    image_count_++;
    return img;
  }

  // ----- LEGO Batman port: render scale -----------------------------------------------------------------------
  // The 3D frame can be drawn below 720p: render targets and resolve destinations are made at hs_/12 (kScaleDen) of their guest
  // size, and every guest pixel coordinate (viewport, scissor, clear and copy rectangles) is scaled at the point it
  // reaches a host image. Nothing reads its screen position in the game's shaders (no gl_FragCoord), textures are
  // sampled with normalized coordinates, and the present filters the front buffer to the screen. Debug server
  // "flag 17 N": scale N/12 (0 = automatic). A change takes effect at the next frame start (all scaled images are
  // dropped and remade).
  uint32_t hs_ = kScaleDen;
  MemBlock scaled_images_;
  std::vector<HostImage*> retired_scaled_;
  bool scaled_full_ = false;
  uint32_t pool_full_logged_ = ~0u;
  static uint32_t ScaleLo(uint32_t v, uint32_t hs) { return uint32_t((uint64_t(v) * hs) / kScaleDen); }
  static uint32_t ScaleHi(uint32_t v, uint32_t hs) { return uint32_t((uint64_t(v) * hs + kScaleDen - 1) / kScaleDen); }
  HostImage* CreateScaled(uint32_t gw, uint32_t gh, DkImageFormat format, uint32_t flags, bool is_depth, uint32_t hs) {
    HostImage* img = CreateImage(std::max(1u, ScaleHi(gw, hs)), std::max(1u, ScaleHi(gh, hs)), format, flags, is_depth, true);
    if (!img) return nullptr;
    img->gw = gw;
    img->gh = gh;
    img->hs = hs;
    return img;
  }
  // A guest rectangle on an image of scale hs: host pixels, clamped to the image.
  static DkScissor ScaleRect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const HostImage* img) {
    const uint32_t hs = img->hs;
    uint32_t x0 = ScaleLo(x, hs), y0 = ScaleLo(y, hs), x1 = ScaleHi(x + w, hs), y1 = ScaleHi(y + h, hs);
    x0 = std::min(x0, img->width); y0 = std::min(y0, img->height);
    x1 = std::min(x1, img->width); y1 = std::min(y1, img->height);
    return DkScissor{x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
  }
  // At a frame start: choose the scale and apply a change by dropping every scaled image; they are remade on use.
  // Automatic by default (debug server "flag 17 0"): down one twelfth while the GPU's frame time (smoothed) is above
  // 31 ms of the 33.3 ms a 30 fps frame has, never below kMinAutoScale; up one twelfth when the next step is predicted under
  // 28 ms (time x pixel ratio, an overestimate: not all GPU work scales with pixels). After a change, 60 frames
  // without another (each change remakes the targets: one hitch). "flag 17 6..12" fixes the scale.
  double gpu_ema_ = 0;
  uint32_t scale_hold_ = 0;
  // History: the floor was 6/8 (960x540) because 5/8 gained little where it was needed (the busiest views stay ~41 ms: per-draw and vertex work)
  // and looked noticeably softer; it also never stepped back up in gameplay (console, 2026-11-10).
  // 8/12 = 480p (the user's choice, 2026-11-10; the busiest views are geometry-bound and gain little below 540p,
  // the medium-busy ones do).
  static constexpr uint32_t kMinAutoScale = 8;
  void ApplyRenderScale() {
#ifndef CVHD_DK_GL
    const int fixed = lbshot::flags[17];
    uint32_t want = hs_;
    if (fixed >= 6 && fixed <= int(kScaleDen)) {
      want = uint32_t(fixed);
    } else if (scale_hold_) {
      scale_hold_--;
    } else if (gpu_ema_ > 31.0 && hs_ > kMinAutoScale) {
      want = hs_ - 1;
    } else if (hs_ < kScaleDen && gpu_ema_ > 0) {
      const double next = gpu_ema_ * double((hs_ + 1) * (hs_ + 1)) / double(hs_ * hs_);
      if (next < 28.0) want = hs_ + 1;
    }
    if (want == hs_ && !scaled_full_) return;
    dkQueueWaitIdle(queue_);
    for (auto& t : targets_) RecycleImage(t.second);
    targets_.clear();
    for (auto it = guest_images_.begin(); it != guest_images_.end();) {
      if (it->second->scaled_pool) { RecycleImage(it->second); it = guest_images_.erase(it); }
      else ++it;
    }
    for (HostImage* r : retired_scaled_) delete r;
    retired_scaled_.clear();
    scaled_images_.Reset();
    scaled_full_ = false;
    bound_color_ = bound_depth_ = nullptr;
    if (want == hs_) { Log("scaled image pool was full: reset at %u/12", hs_); return; }
    Log("render scale %u/12 -> %u/12 (GPU %.1f ms/frame)", hs_, want, gpu_ema_);
    hs_ = want;
    scale_hold_ = 60;
    gpu_ema_ = 0;  // measured again at the new scale
#endif
  }

  // Fresh pool memory holds whatever an earlier owner left (noise on screen where the game's clears do
  // not reach). Render targets and resolve destinations start cleared to zero instead; textures are
  // fully uploaded, and block-compressed images cannot be cleared.
  // GPU time per work category (flag 6): a timestamp where the category changes; the time to the next mark is
  // counted for the category. The reports wait for earlier work, so the totals include some serialisation.
  // LEGO Batman port (perf): per-draw state is sent only when it differs from the last draw's (the GPU decodes every
  // method; ~1400 draws re-sent the same rasterizer/blend/depth/shader state). Reset at a frame start and around the
  // present, which binds its own state. Debug server "flag 10 1" sends everything again (A/B).
  // Deferred barrier after a resolve copy (see Resolve).
  bool resolve_barrier_pending_ = false;
  uint64_t last_resolve_sig_[5] = {};
  uint64_t last_resolve_work_ = ~0ull;
  uint64_t resolve_clears_ = 0;
  uint32_t resolves_skipped_ = 0;
  void FlushResolveBarrier(DkCmdBuf cmd) {
    if (!resolve_barrier_pending_) return;
    resolve_barrier_pending_ = false;
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
  }
  void FlushResolveBarrier() { if (frames_[frame_].cmdbuf) FlushResolveBarrier(frames_[frame_].cmdbuf); }

  enum StateSlot { kSRast, kSColor, kSColorWrite, kSBlend, kSDepth, kSStencil, kSShaders, kSVtxAttr, kSVtxBuf, kSDepthBias, kSlotCount };
  std::string state_cache_[kSlotCount];
  bool StateChanged(int slot, const void* p, size_t n) {
#ifndef CVHD_DK_GL
    if (lbshot::flags[10]) return true;
#endif
    std::string& c = state_cache_[slot];
    if (c.size() == n && std::memcmp(c.data(), p, n) == 0) return false;
    c.assign(static_cast<const char*>(p), n);
    return true;
  }
  void StateCacheReset() { for (auto& c : state_cache_) c.clear(); }
  // Debug server "flag 9 1": every draw with zero instances (state and commands only; measurement).
  static uint32_t DrawInstances() {
#ifndef CVHD_DK_GL
    if (lbshot::flags[9]) return 0;
#endif
    return 1;
  }

  enum GpuTag : uint8_t { kTagDraw, kTagClear, kTagResolve, kTagUpload, kTagTarget, kTagPresent, kTagCount };
  // Per-shader GPU time (flag 14). Special hashes: 1 resolve, 2 clear, 3 frame start, 4 present.
  void PsMark(Frame& f, uint64_t hash) {
    if (f.pm_off == UINT32_MAX || f.pm_n >= Frame::kMaxPsMarks) return;
    dkCmdBufReportCounter(f.cmdbuf, DkCounter_Timestamp, f.staging.gpu + f.pm_off + 16 * f.pm_n);
    f.pm_hash[f.pm_n++] = hash;
  }
  struct PsCost { double ms = 0; uint32_t draws = 0; };
  std::unordered_map<uint64_t, PsCost> ps_cost_;
  uint32_t ps_cost_frames_ = 0;

  void GpuMark(Frame& f, uint8_t tag) {
    if (f.mk_off == UINT32_MAX || f.mk_n >= Frame::kMaxMarks) return;
    if (f.mk_n && f.mk_tag[f.mk_n - 1] == tag) return;
    dkCmdBufReportCounter(f.cmdbuf, DkCounter_Timestamp, f.staging.gpu + f.mk_off + 16 * f.mk_n);
    f.mk_tag[f.mk_n++] = tag;
  }
  struct GpuScope {
    Deko3dCommandProcessor* b; Frame& f;
    GpuScope(Deko3dCommandProcessor* b_, Frame& f_, uint8_t tag) : b(b_), f(f_) { b->GpuMark(f, tag); }
    ~GpuScope() { b->GpuMark(f, kTagDraw); }
  };
  double t_cat_ms_[kTagCount] = {};
  uint32_t t_cat_marks_ = 0;

  void ClearNewImage(HostImage* img) {
    if (!img || !frames_[frame_].cmdbuf) return;
    FlushResolveBarrier();
    GpuScope gs(this, frames_[frame_], kTagClear);
    StateCacheReset();  // deko3d clears/copies may change bound state
    switch (img->format) {
      case DkImageFormat_RGBA_BC1: case DkImageFormat_RGBA_BC2: case DkImageFormat_RGBA_BC3: case DkImageFormat_RG_BC5_Unorm:
      case DkImageFormat_R_BC4_Unorm: return;
      default: break;
    }
    DkCmdBuf cmd = frames_[frame_].cmdbuf;
    DkScissor sc{0, 0, img->width, img->height};
    if (img->is_depth) {
      BindTargets(cmd, nullptr, img);
      dkCmdBufSetScissors(cmd, 0, &sc, 1);
      dkCmdBufClearDepthStencil(cmd, true, 1.0f, 0xFF, 0);
    } else {
      const float zero[4] = {0, 0, 0, 0};
      BindTargets(cmd, img, nullptr);
      dkCmdBufSetScissors(cmd, 0, &sc, 1);
      dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, zero);
    }
  }

  static uint32_t EffectivePitch(reg::RB_SURFACE_INFO surf) {
    // The XDK clears through a 4x MSAA alias at half the pitch (2x2 samples per pixel: the same
    // EDRAM bytes as the 1x surface, both axes halved).
    return surf.msaa_samples == xenos::MsaaSamples::k4X ? surf.surface_pitch * 2 : surf.surface_pitch;
  }

  HostImage* TargetImage(uint32_t edram_base, uint32_t pitch, DkImageFormat format, bool is_depth) {
    if (!pitch) return nullptr;
    uint64_t key = (uint64_t(edram_base) << 32) | (uint64_t(pitch) << 1) | (is_depth ? 1 : 0);
    auto it = targets_.find(key);
    if (it != targets_.end()) return it->second;
    uint32_t tiles = edram_base < kEdramTiles ? kEdramTiles - edram_base : 0;
    uint32_t rows = tiles ? uint32_t((uint64_t(tiles) * 80 * 16) / pitch) : 0;
    if (rows > kMaxTargetHeight) rows = kMaxTargetHeight;
    // LEGO Batman port (perf): at least a full 720p screen, so a single pass over three 256-row tiles fits targets
    // whose EDRAM base leaves fewer rows (the depth at 1536: 512 rows, the cutscenes' black lower area). Tiled
    // drawing never reaches those rows.
    if (rows < 736) rows = 736;
    // LEGO Batman port (perf, 2026-11-10): render targets with hardware compression (colour and depth compression, fast
    // clears): per-shader GPU timing showed 15 clears at ~200 us each and bandwidth-bound draws. The file
    // sdmc:/switch/legobatman/nocompress turns it off (read at start).
    HostImage* img = CreateScaled(pitch, rows, format, DkImageFlags_UsageRender | (target_compression_ ? DkImageFlags_HwCompression : 0u),
                                  is_depth, hs_);
    if (!img) return nullptr;
    img->edram_base = edram_base;
    img->edram_pitch = pitch;
    targets_.emplace(key, img);
    ClearNewImage(img);
    Log("%s target: EDRAM base %u pitch %u -> %ux%u image, host format %u (%u MB in images)", is_depth ? "depth" : "color", edram_base, pitch,
        img->width, img->height, uint32_t(format), images_.used >> 20);
    return img;
  }

  void BindTargets(DkCmdBuf cmd, HostImage* color, HostImage* depth) {
    if (color == bound_color_ && depth == bound_depth_) return;
    GpuScope gs(this, frames_[frame_], kTagTarget);
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, 0);
    DkImageView cv, dv;
    DkImageView const* colors[1] = {&cv};
    if (color) dkImageViewDefaults(&cv, &color->image);
    if (depth) dkImageViewDefaults(&dv, &depth->image);
    dkCmdBufBindRenderTargets(cmd, color ? colors : nullptr, color ? 1 : 0, depth ? &dv : nullptr);
    bound_color_ = color;
    bound_depth_ = depth;
  }

  // The image at a guest address (resolve destinations, textures). A fetch or a swap asks for the
  // exact size (a resolve created it with the aligned height: resized once, contents copied); a
  // resolve asks for a minimum size (`at_least`) and creates with the aligned size (`create_w/h`).
  // width/height and create_w/h are guest pixels; hs: the scale a new image gets (8 = 1:1; resolves pass hs_).
  HostImage* GuestImage(Frame& f, uint32_t base, uint32_t width, uint32_t height, DkImageFormat format, bool at_least,
                        uint32_t create_w = 0, uint32_t create_h = 0, uint32_t hs = 0) {
    auto it = guest_images_.find(base);
    if (it != guest_images_.end()) {
      HostImage* img = it->second;
      // A resolve into an image of another scale (an uploaded texture's memory, or one made before a scale change):
      // remade at the resolve's scale, as for a format change (the copy cannot rescale).
      if (img->format != format || (hs != 0 && (img->hs != hs || !img->scaled_pool))) {
        // The memory was reused with another format (a texture over an old resolve destination or the
        // reverse): a new image. ponytail: the old image's pool memory is not reclaimed (bump pool);
        // a free list when the pool fills in a stage.
        HostImage* fresh = hs ? CreateScaled(std::max(width, create_w), std::max(height, create_h), format, DkImageFlags_UsageRender, false, hs)
                              : CreateImage(std::max(width, create_w), std::max(height, create_h), format, DkImageFlags_UsageRender, false);
        if (!fresh) return nullptr;
        fresh->guest_base = base;
        if (create_w || create_h) ClearNewImage(fresh);
        Log("image at %08X recreated: format %u -> %u, %ux%u -> %ux%u", base, uint32_t(img->format), uint32_t(format), img->width,
            img->height, fresh->width, fresh->height);
        it->second = fresh;
        RecycleImage(img);
        return fresh;
      }
      if (img->gw == width && img->gh == height) return img;
      if (at_least && width <= img->gw && height <= img->gh) return img;
      if (at_least) {
        width = std::max(width, img->gw);
        height = std::max(height, img->gh);
      }
      HostImage* resized = img->scaled_pool ? CreateScaled(width, height, img->format, DkImageFlags_UsageRender, false, img->hs)
                                            : CreateImage(width, height, img->format, DkImageFlags_UsageRender, false);
      if (!resized) return img;
      if (!img->from_resolve) {
        // An uploaded texture whose memory now holds a texture of another size (the game reuses sprite memory:
        // 20-30 size changes per address in a stage): the tiled layout differs, so the old texels are noise.
        // Upload from guest memory again (content_serial 0) instead of copying. A resolve into it (at_least) writes a
        // sub-rectangle: clear the rest (a recycled image holds old texels).
        if (at_least) ClearNewImage(resized);
        resized->guest_base = base;
        it->second = resized;
        RecycleImage(img);
        return resized;
      }
      ClearNewImage(resized);
      uint32_t cw = std::min(resized->width, img->width), ch = std::min(resized->height, img->height);  // host pixels, same scale
      DkImageView sv, dv;
      dkImageViewDefaults(&sv, &img->image);
      dkImageViewDefaults(&dv, &resized->image);
      DkImageRect sr{0, 0, 0, cw, ch, 1}, dr{0, 0, 0, cw, ch, 1};
      dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
      dkCmdBufCopyImage(f.cmdbuf, &sv, &sr, &dv, &dr, 0);
      dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
      resized->from_resolve = img->from_resolve;
      resized->content_serial = img->content_serial;
      resized->checked_serial = img->checked_serial;
      resized->guest_base = base;
      Log("image at %08X resized %ux%u -> %ux%u", base, img->width, img->height, width, height);
      it->second = resized;
      RecycleImage(img);  // gh: its pool memory is reused by the next request of its size
      return resized;
    }
    HostImage* img = hs ? CreateScaled(std::max(width, create_w), std::max(height, create_h), format, DkImageFlags_UsageRender, false, hs)
                        : CreateImage(std::max(width, create_w), std::max(height, create_h), format, DkImageFlags_UsageRender, false);
    if (!img) return nullptr;
    img->guest_base = base;
    guest_images_.emplace(base, img);
    if (create_w || create_h) ClearNewImage(img);
    return img;
  }

  // LEGO Batman port: why draws were skipped this frame (printed with the swap line).
  uint32_t frame_skip_why_[8] = {};
  uint32_t frame_biased_ = 0, frame_stenciled_ = 0;
  // ----- LEGO Batman port: single-pass rendering of the game's EDRAM tiles ---------------------------------------
  // The 360's 10 MB EDRAM forces the game (D3D9 predicated tiling) to draw a 1280x720 frame twice: the top tile
  // (rows 0-511) and the bottom tile (window offset y = -512, rows 512-719), each draw that touches both tiles twice.
  // The host target has no such limit, so: top-tile draws get the full window scissor (no screen scissor, which only
  // tiling uses), bottom-tile draws identical to a top-tile draw of the frame are skipped, the others are drawn
  // without the window offset (at their final rows), and the bottom tile's resolve reads those rows. Console
  // 2026-11-10: skipping the bottom tile took the GPU from 37 to 21 ms and the backend from 19 to 12 ms per frame.
  bool tiling_seen_ = false;               // a draw with a window offset happened (this or an earlier frame)
  std::unordered_set<uint64_t> tile_keys_;  // keys of this frame's top-tile draws
  uint32_t frame_tile_dups_ = 0;            // bottom-tile draws skipped as duplicates this frame
  uint32_t frame_tile_dependent_ = 0;       // draws kept per tile: a tile's post pass or a tile-resolve reader
  int cur_tile_off_ = 0;                    // window y offset of the tile being drawn
  bool tile_post_ = false;                  // that tile has resolved: its remaining draws are its post pass
  bool prev_frame_tiled_ = false;           // the last frame drew lower tiles (this one will too, until they appear)
  uint32_t tile_h_ = 0, frame_tile_h_ = 0;  // the top tile's height: the smallest window y offset (256 or 512 here)
  bool frame_offset_draws_ = false;  // this frame drew lower tiles through a window offset (some scenes resolve in
                                     // tile-sized pieces from one full-height drawing instead)
  bool SinglePass() const {
#ifndef CVHD_DK_GL
    if (lbshot::flags[2]) return false;  // debug server "flag 2 1": off, for A/B
#endif
#ifdef CVHD_DK_GL
    static const bool off = std::getenv("LB_NO_SINGLEPASS") != nullptr;  // GL host A/B
    if (off) return false;
#endif
    // Only the two-tile layout (top tile 512 rows: gameplay). With 256-row tiles (cutscenes: three tiles, MRTs at EDRAM
    // bases 0/512/1024, depth at 1536) the EDRAM-sized targets are too short for a full-height pass.
    // Debug server "flag 12 1": also the three-tile layout (256-row tiles; targets are now at least 736 rows).
#ifndef CVHD_DK_GL
    if (lbshot::flags[12]) return kSinglePassTiles && tiling_seen_ && tile_h_ >= 256;
#endif
    return kSinglePassTiles && tiling_seen_ && tile_h_ >= 512;
  }
  uint64_t DrawKey(const RegisterFile& regs, DkShaderObj* vs, DkShaderObj* ps, xenos::PrimitiveType prim, uint32_t count,
                   IndexBufferInfo* ib) const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(vs->ucode_data_hash()); mix(ps->ucode_data_hash()); mix(uint32_t(prim)); mix(count);
    if (ib) { mix(ib->guest_base); mix(ib->count); mix(uint32_t(ib->format)); }
    for (uint32_t r = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0; r < XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 192; r++) mix(regs.values[r]);
    for (int stage = 0; stage < 2; stage++) {
      const DkShaderObj* sh = stage ? ps : vs;
      const uint32_t base = stage ? XE_GPU_REG_SHADER_CONSTANT_256_X : XE_GPU_REG_SHADER_CONSTANT_000_X;
      const auto& map = sh->constant_register_map();
      for (int i = 0; i < 256; i++)
        if ((map.float_bitmap[i >> 6] >> (i & 63)) & 1)
          for (int c = 0; c < 4; c++) mix(regs.values[base + i * 4 + c]);
    }
    for (uint32_t r = XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031; r < XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + 8; r++) mix(regs.values[r]);
    for (uint32_t r = XE_GPU_REG_SHADER_CONSTANT_LOOP_00; r < XE_GPU_REG_SHADER_CONSTANT_LOOP_00 + 32; r++) mix(regs.values[r]);
    for (uint32_t r : {uint32_t(XE_GPU_REG_RB_COLORCONTROL), uint32_t(XE_GPU_REG_RB_DEPTHCONTROL), uint32_t(XE_GPU_REG_RB_BLENDCONTROL0),
                       uint32_t(XE_GPU_REG_RB_COLOR_MASK), uint32_t(XE_GPU_REG_PA_SU_SC_MODE_CNTL), uint32_t(XE_GPU_REG_RB_COLOR_INFO),
                       uint32_t(XE_GPU_REG_RB_DEPTH_INFO), uint32_t(XE_GPU_REG_RB_STENCILREFMASK), uint32_t(XE_GPU_REG_RB_SURFACE_INFO),
                       uint32_t(XE_GPU_REG_PA_CL_VTE_CNTL), uint32_t(XE_GPU_REG_PA_CL_VPORT_XSCALE), uint32_t(XE_GPU_REG_PA_CL_VPORT_YSCALE),
                       uint32_t(XE_GPU_REG_PA_CL_VPORT_XOFFSET), uint32_t(XE_GPU_REG_PA_CL_VPORT_YOFFSET)})
      mix(regs.values[r]);
    return h;
  }
  uint32_t vid_log_ = 0;  // movie plane log lines written
  // LEGO Batman port (console flicker hunt): the frame structure of two windows of swaps, to compare console and PC.
  bool FrameLogOn() const {
    const uint32_t n = swaps_ + 1;
#ifdef CVHD_DK_GL
    static const uint32_t at = std::getenv("LB_FRAME_LOG") ? uint32_t(std::atoi(std::getenv("LB_FRAME_LOG"))) : 0;
    if (at) return n >= at && n < at + 2;
#endif
    // Only on request (debug server "flag 11 N": the next N frames). The fixed windows at swaps 480-519 and
    // 1380-1419 made the intro crawl for ~2 s (a log line per draw to the SD card).
    return n < frame_log_until_;
  }
  uint32_t frame_log_until_ = 0;
  static unsigned ImgId(const void* p) { return unsigned((reinterpret_cast<uintptr_t>(p) >> 4) & 0xFFFFF); }  // draws with depth bias / stencil this frame
  uint32_t trace_min_draws_ = 440;
  void SkipDraw(int why) { skipped_++; frame_skipped_++; frame_skip_why_[why]++; }

  HostImage* placeholders_[2] = {};  // [0] black (cube maps), [1] white (unsupported formats: depth 1.0)
  HostImage* PlaceholderTexture(bool white = true) {
    HostImage*& ph = placeholders_[white ? 1 : 0];
    if (ph) return ph;
    if (!frames_[frame_].cmdbuf) return nullptr;
    ph = CreateImage(8, 8, DkImageFormat_RGBA8_Unorm, DkImageFlags_UsageRender, false);
    if (!ph) return nullptr;
    ph->from_resolve = true;  // host-native: no fetch swizzle
    ph->content_serial = serial_;
    DkCmdBuf cmd = frames_[frame_].cmdbuf;
    BindTargets(cmd, ph, nullptr);
    DkScissor sc{0, 0, 8, 8};
    dkCmdBufSetScissors(cmd, 0, &sc, 1);
    const float v = white ? 1.0f : 0.0f;
    const float c[4] = {v, v, v, 1};
    dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, c);
    return ph;
  }

  HostImage* TextureImage(Frame& f, const TextureInfo& info, const xenos::xe_gpu_texture_fetch_t& fetch) {
    DkImageFormat format;
    bool reversed;
    if (!TextureFormatOf(info.format, format, reversed)) {
      // LEGO Batman port: bind a white placeholder instead of failing the fetch. Skipping the draw lost the
      // game's final composite every frame (it copies the scene colour and restores depth from a k_24_8 depth
      // resolve through a second fetch): the screen stayed black. White = depth 1.0 (far) for depth formats.
      LogOnce((std::string("tfmt ") + info.format_info()->name).c_str(), "unsupported texture format %s: white placeholder bound",
              info.format_info()->name);
      return PlaceholderTexture();
    }
    // TextureInfo keeps the fetch constant's fields: the size minus one.
    if (info.width + 1 >= 4000) frame_big_tex_ = true;
    const TexSource src = SourceOf(info);
    if (src.level) frame_mip_only_ = true;
    const uint32_t base = src.address;
    HostImage* img = GuestImage(f, base, src.width, src.height, format, false);
    if (!img) return nullptr;
    if (img->checked_serial == serial_) return img;
    img->reversed_channels = reversed;  // already checked for this draw
    img->checked_serial = serial_;
#ifdef LEGOBATMAN_HASH_TEXTURES
    // LEGO Batman port: hash at most once per image per frame (the serial advances per draw).
    if (img->content_serial && img->checked_frame == swaps_) return img;
    img->checked_frame = swaps_;
#endif
    // Never filled, or written by the guest (or a file read) after the upload / resolve that filled it.
    if (img->content_serial && !MemoryChanged(*img, base, src.bytes)) return img;
    // LEGO Batman port: CPU-written (linear) images and ones that changed before (the intro movie's YUV planes: the
    // decoder rewrites only some macroblocks, which the sampled hash missed) are hashed in full. The hash is taken
    // BEFORE the copy: a decoder still writing during the copy then leaves memory that differs from the hash, so the
    // next frame copies again (hashing after the copy matched the finished frame and kept the torn copy).
    const bool full = !info.is_tiled || img->content_serial || img->full_hash;
    const uint64_t hash_before = HashMemory(base, src.bytes, full);
    // A scaled image (a resolve destination at the render scale) holds GPU-made texels at another size: never
    // filled from guest memory.
    if (img->hs != kScaleDen) return img;
    // A failed re-upload (staging ring full) keeps the previous contents rather than unbinding the slot.
    if (!UploadTexture(f, *img, info, src, fetch.endianness)) return img->content_serial ? img : nullptr;
    img->full_hash = full;
    if (img->content_serial) {
      reuploads_++;
      frame_reuploads_++;
      if (reuploads_ <= 8) Log("texture at %08X re-uploaded (%s, %ux%u): its memory changed", base,
                               img->from_resolve ? "was a resolve destination" : "was uploaded before", img->width, img->height);
    }
    img->from_resolve = false;
    img->content_serial = serial_;
    img->mem_bytes = src.bytes;
    img->mem_hash = hash_before;
    WatchRange(base, img->mem_bytes);  // LEGO Batman port: re-armed after every upload
    frame_uploaded_.push_back({img->guest_base, img->mem_bytes, img->content_serial});
#ifdef CVHD_DK_GL
    QueueTextureDump(img, info);
#endif
    return img;
  }

#ifdef CVHD_DK_GL
  // Linux test host: every uploaded texture (first 400, and re-uploads of the first 60 images) is written
  // as a PPM after the frame's commands ran, named by guest address, size, format and upload count.
  void QueueTextureDump(HostImage* img, const TextureInfo& info) {
    static const char* dir = std::getenv("CVHD_DK_DUMP_DIR");
    if (!dir || dump_count_ >= 400) return;
    uint32_t n = ++upload_counts_[img->guest_base];
    if (n > 3) return;
    char name[256];
    std::snprintf(name, sizeof(name), "%s/tex-%08X-%ux%u-%s-%s%u.ppm", dir, img->guest_base, img->width, img->height, info.format_info()->name,
                  info.is_tiled ? "t" : "l", n);
    pending_dumps_.emplace_back(img, name);
    dump_count_++;
  }
  void FlushTextureDumps() {
    for (auto& d : pending_dumps_) dkglDumpImage(&d.first->image, d.second.c_str());
    pending_dumps_.clear();
    // CVHD_DK_DUMP_AT=2900,3000: at those swaps every render target and resolve/texture image is written
    // (after the swap's commands ran).
    // Or on demand: creating the file <dump dir>/dump_now dumps the next swap (the file is removed).
    static const char* at = std::getenv("CVHD_DK_DUMP_AT");
    static const char* dir = std::getenv("CVHD_DK_DUMP_DIR");
    if (!dir) return;
    std::string trigger = std::string(dir) + "/dump_now";
    bool now = (swaps_ & 7) == 0 && access(trigger.c_str(), F_OK) == 0;
    if (now) unlink(trigger.c_str());
    if (!now) {
      if (!at) return;
      std::string list = std::string(",") + at + ",";
      if (list.find("," + std::to_string(swaps_ + 1) + ",") == std::string::npos) return;
    }
    char name[256];
    for (auto& it : targets_) {
      if (it.second->is_depth) continue;
      std::snprintf(name, sizeof(name), "%s/at%u-target-base%u-pitch%u.ppm", dir, swaps_ + 1, it.second->edram_base, it.second->edram_pitch);
      dkglDumpImage(&it.second->image, name);
    }
    for (auto& it : guest_images_) {
      if (it.second->width < 64) continue;
      std::snprintf(name, sizeof(name), "%s/at%u-guest-%08X-%ux%u%s.ppm", dir, swaps_ + 1, it.first, it.second->width, it.second->height,
                    it.second->from_resolve ? "-resolve" : "");
      dkglDumpImage(&it.second->image, name);
    }
  }
  std::vector<std::pair<HostImage*, std::string>> pending_dumps_;
  std::unordered_map<uint32_t, uint32_t> upload_counts_;
  uint32_t dump_count_ = 0;
#endif

  // Where the largest stored level of a texture lives. A fetch without a base level (base address 0, or a minimum
  // mip level above 0: the game streamed it out) keeps its data under the mip address: the full-screen map and the
  // stage overview draw the 4320x2432 castle layers so, and an upload from address 0 drew nothing (console 0.177).
  // The host image is then that mip at its own size; the shader's normalized coordinates sample it the same.
  struct TexSource {
    uint32_t address, bytes, width, height, pitch_blocks, ox, oy, level;
  };
  static TexSource SourceOf(const TextureInfo& info) {
    const uint32_t w0 = info.width + 1, h0 = info.height + 1;  // TextureInfo keeps size minus one
    // Row pitch in blocks, tile-aligned (TextureInfo::pitch is in texels: 4x too wide for DXT).
    TexSource s{info.memory.base_address, info.memory.base_size, w0, h0, info.extent.block_pitch_h, 0, 0, 0};
    uint32_t oz = 0;
    if (info.memory.base_address || !info.memory.mip_address) {
      // A texture whose shorter side is 16 texels or less keeps its base level in the packed mip tail: at an offset
      // inside its 32x32 tile (x 16 for a 16x24 sprite). Read from 0,0, the HUD digits and 16-wide ledge strips were noise.
      if (info.has_packed_mips) texture_util::GetPackedMipOffset(w0, h0, 1, info.format, 0, s.ox, s.oy, oz);
      return s;
    }
    s.level = std::max(1u, info.mip_min_level);
    const auto layout = texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, info.pitch >> 5, w0, h0, 1,
                                                            info.is_tiled, info.format, info.has_packed_mips, false,
                                                            std::max(s.level, info.mip_max_level));
    const uint32_t stored = std::min(s.level, layout.packed_level);  // levels from the packed one on share its tail
    s.address = info.memory.mip_address + layout.mip_offsets_bytes[stored];
    s.bytes = layout.mips[stored].level_data_extent_bytes;
    s.width = std::max(1u, w0 >> s.level);
    s.height = std::max(1u, h0 >> s.level);
    s.pitch_blocks = layout.mips[stored].row_pitch_bytes / info.format_info()->bytes_per_block();
    if (s.level >= layout.packed_level) texture_util::GetPackedMipOffset(w0, h0, 1, info.format, s.level, s.ox, s.oy, oz);
    return s;
  }

  bool UploadTexture(Frame& f, HostImage& img, const TextureInfo& info, const TexSource& s, xenos::Endian endian) {
    FlushResolveBarrier(f.cmdbuf);
    GpuScope gs(this, f, kTagUpload);
    const FormatInfo* fi = info.format_info();
    const uint32_t width = s.width, height = s.height;
    uint32_t bw = (width + fi->block_width - 1) / fi->block_width;
    uint32_t bh = (height + fi->block_height - 1) / fi->block_height;
    uint32_t bpb = fi->bytes_per_block();
    uint32_t bytes = bw * bh * bpb;
    uint32_t off = f.staging.Alloc(bytes, 0x100);
    if (off == UINT32_MAX) {
      static uint32_t full_logged = 0;
      if (!ring_full_swap_) ring_full_swap_ = swaps_ + 1;
      if (full_logged++ < 60) Log("staging ring full at swap #%u (%u KB used of %u KB): %u KB texture %08X not uploaded", swaps_ + 1, f.staging.used >> 10, kStagingBytes >> 10, bytes >> 10, s.address);
      return false;
    }
    uint8_t* dst = f.staging.cpu + off;
    const uint8_t* src = memory_->TranslatePhysical(s.address);
    if (info.is_tiled) {
      if (!::cvhd::UntileFast(dst, src, bw, bh, s.pitch_blocks, bpb, static_cast<::cvhd::SwapMode>(uint32_t(endian)), s.ox, s.oy)) {
        LogOnce("untile", "no untile for %u-byte blocks (%s)", bpb, fi->name);
        return false;
      }
    } else {
      uint32_t row = bw * bpb;
      uint32_t src_row = s.pitch_blocks * bpb;
      src += s.oy * src_row + s.ox * bpb;
      for (uint32_t y = 0; y < bh; y++) texture_conversion::CopySwapBlock(endian, dst + y * row, src + y * src_row, row);
    }
    DkCopyBuf cb{f.staging.gpu + off, 0, 0};
    DkImageView view;
    dkImageViewDefaults(&view, &img.image);
    DkImageRect rect{0, 0, 0, width, height, 1};
    // A re-upload (the game reuses sprite memory between draws) or a recycled image: earlier draws of this frame
    // may still be sampling it.
    dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, 0);
    dkCmdBufCopyBufferToImage(f.cmdbuf, &cb, &view, &rect, 0);
    dkCmdBufBarrier(f.cmdbuf, DkBarrier_Full, DkInvalidateFlags_Image);
    uploads_++;
    upload_bytes_ += bytes;
    if (uploads_ <= 8) Log("texture %s %ux%u %s endian %u at %08X: %u KB uploaded", fi->name, width, height,
                           info.is_tiled ? "tiled" : "linear", uint32_t(endian), s.address, bytes >> 10);
    return true;
  }

  uint32_t DescriptorFor(Frame& f, HostImage* img, uint32_t swizzle) {
    uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(img)) ^ (uint64_t(swizzle) << 48);
    auto it = f.descriptors.find(key);
    if (it != f.descriptors.end()) return it->second;
    if (f.next_descriptor >= kMaxImageDescriptors) { LogOnce("desc", "image descriptors exhausted"); return UINT32_MAX; }
    DkImageView view;
    dkImageViewDefaults(&view, &img->image);
    if (swizzle) {
      view.swizzle[0] = SwizzleOf(swizzle);
      view.swizzle[1] = SwizzleOf(swizzle >> 3);
      view.swizzle[2] = SwizzleOf(swizzle >> 6);
      view.swizzle[3] = SwizzleOf(swizzle >> 9);
    }
    DkImageDescriptor desc;
    dkImageDescriptorInitialize(&desc, &view, false, false);
    uint32_t slot = f.next_descriptor++;
    std::memcpy(descriptors_.cpu + f.desc_offset + slot * sizeof(DkImageDescriptor), &desc, sizeof(desc));
    // The GPU caches texture headers: a slot rewritten by the CPU must be invalidated before the draw.
    // LEGO Batman port (perf): not needed per slot: the frame is submitted once, at the swap, so every slot is written
    // before the GPU starts the frame, and BeginFrame's barrier invalidates the descriptor caches. Debug server
    // "flag 8 1" restores the per-slot invalidation (A/B).
#ifndef CVHD_DK_GL
    if (lbshot::flags[8])
#endif
      dkCmdBufBarrier(f.cmdbuf, DkBarrier_None, DkInvalidateFlags_Descriptors);
    f.descriptors.emplace(key, slot);
    return slot;
  }

  // ----- draws ----------------------------------------------------------------------------------
  static uint64_t SampleCheck(const uint8_t* p, uint32_t bytes) {
    uint64_t h = bytes;
    const uint32_t n = bytes / 8;
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    if (bytes <= 65536) {
      // LEGO Batman port: small buffers (particles, HUD, text: the dynamic ones) are checked word by word; an
      // 8-word sample could match a rewritten buffer and draw stale vertices for a frame.
      for (uint32_t i = 0; i < n; i++) h = (h ^ w[i]) * 1099511628211ull;
      for (uint32_t i = n * 8; i < bytes; i++) h = (h ^ p[i]) * 1099511628211ull;
      return h;
    }
    for (uint32_t k = 0; k < 64; k++) h = (h ^ w[(uint64_t(n - 1) * k) / 63]) * 1099511628211ull;
    return h;
  }
  // ----- LEGO Batman port (perf, 2026-11-10): vertex and index data kept across frames ----------------------------
  // Every frame byte-swapped all vertex data into the ring again (~10 ms/frame of the GPU command thread at ~1200
  // draws) and copied + scanned the indices of every draw. Data whose bytes are the same as in the last frame now
  // stays in persist_ (GPU-cached; BeginFrame's L2 invalidation covers reuse of addresses after a reset):
  //  - checked once per frame by a full hash (cheaper than the swap-copy), then reused by every draw of the frame;
  //  - a changed buffer gets new space, never rewritten in place (an earlier frame may still read the old bytes);
  //  - one whose bytes changed twice is "dynamic" and goes through the per-frame ring as before;
  //  - the pool is emptied at a frame start when 3/4 full (after the GPU is idle).
  // Debug server "flag 7 1" turns it off (A/B).
  static constexpr uint32_t kPersistBytes = 64u << 20;
  struct PEntry {
    uint32_t off = 0, bytes = 0;  // bytes held (vertices: the largest range asked for)
    uint64_t hash = 0;
    uint64_t sample = 0;          // SampleCheck at the last check: later uses in the same frame compare it
    uint32_t checked = 0;         // swaps_ + 1 of the last check
    uint32_t max_index = 0;       // index buffers
    uint8_t changes = 0;
  };
  MemBlock persist_;
  bool persist_ok_ = false;
  bool target_compression_ = true;
  std::unordered_map<uint64_t, PEntry> persist_vtx_, persist_idx_;
  uint32_t persist_hits_ = 0, persist_copies_ = 0, persist_dynamic_ = 0, persist_resets_ = 0;

  static uint64_t FullHash(const uint8_t* p, uint32_t bytes) {
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    const uint32_t n = bytes / 8;
    uint64_t a = 0x9E3779B97F4A7C15ull ^ bytes, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {  // four independent lanes: the multiplies overlap
      a = (a ^ w[i]) * 0x100000001B3ull;
      b = (b ^ w[i + 1]) * 0x100000001B3ull;
      c = (c ^ w[i + 2]) * 0x100000001B3ull;
      d = (d ^ w[i + 3]) * 0x100000001B3ull;
    }
    for (; i < n; i++) a = (a ^ w[i]) * 0x100000001B3ull;
    for (uint32_t k = n * 8; k < bytes; k++) b = (b ^ p[k]) * 0x100000001B3ull;
    return a ^ (b * 31) ^ (c * 131) ^ (d * 1031);
  }

  // Re-check of a buffer used again in the same frame: every word up to 4 KB (HUD text, particles: the buffers a
  // game rewrites between batches are small), 64 sampled words above (SampleCheck hashed up to 64 KB in full on every
  // draw: ~10% of the GPU thread).
  static uint64_t QuickCheck(const uint8_t* p, uint32_t bytes) {
    if (bytes <= 4096) return SampleCheck(p, bytes);
    uint64_t h = bytes;
    const uint32_t n = bytes / 8;
    const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
    for (uint32_t k = 0; k < 64; k++) h = (h ^ w[(uint64_t(n - 1) * k) / 63]) * 1099511628211ull;
    return h;
  }

  bool PersistOn() const {
#ifndef CVHD_DK_GL
    if (lbshot::flags[7]) return false;
#endif
    return persist_ok_;
  }

  // GPU address of `bytes` of guest data in the persistent pool, or 0 = use the ring. `fill(dst, src, bytes, &max)`
  // writes the converted bytes; `max_index` receives the cached maximum index of an index buffer.
  template <typename Fill>
  DkGpuAddr PersistLookup(std::unordered_map<uint64_t, PEntry>& map, uint64_t key, const uint8_t* src, uint32_t bytes, Fill fill,
                          uint32_t* max_index = nullptr) {
    if (!PersistOn() || bytes == 0 || bytes > (8u << 20)) return 0;
    auto [it, inserted] = map.try_emplace(key);
    PEntry& e = it->second;
    if (e.changes >= 2) return 0;  // dynamic
    const uint32_t now = swaps_ + 1;
    if (!inserted && e.bytes >= bytes && e.checked == now) {
      // Used again in the same frame: rewritten since the check (a buffer reused for several batches)? Then dynamic.
      if (QuickCheck(src, e.bytes) != e.sample) { e.changes = 2; persist_dynamic_++; return 0; }
      persist_hits_++;
      if (max_index) *max_index = e.max_index;
      return persist_.gpu + e.off;
    }
    const uint32_t want = std::max(bytes, inserted ? 0u : e.bytes);
    const uint64_t h = FullHash(src, want);
    if (!inserted && e.bytes == want && e.hash == h) {
      e.checked = now;
      e.sample = QuickCheck(src, want);
      persist_hits_++;
      if (max_index) *max_index = e.max_index;
      return persist_.gpu + e.off;
    }
    if (!inserted && e.bytes == want && ++e.changes >= 2) { persist_dynamic_++; return 0; }  // rewritten again
    const uint32_t off = persist_.Alloc(want, 256);
    if (off == UINT32_MAX) { if (inserted) map.erase(it); return 0; }
    uint32_t mi = 0;
    fill(persist_.cpu + off, src, want, &mi);
    e.off = off;
    e.bytes = want;
    e.hash = h;
    e.sample = QuickCheck(src, want);
    e.checked = now;
    e.max_index = mi;
    persist_copies_++;
    if (max_index) *max_index = mi;
    return persist_.gpu + off;
  }

  // At a frame start: empty the pool when 3/4 full (every frame that used it must be finished first).
  void PersistFrameStart() {
    if (!persist_ok_ || persist_.used < persist_.size / 4 * 3) return;
    dkQueueWaitIdle(queue_);
    persist_.Reset();
    persist_vtx_.clear();
    persist_idx_.clear();
    persist_resets_++;
  }

  template <typename T>
  static uint32_t SwapIndices(T* d, const T* s, uint32_t count, xenos::Endian endian) {
    uint32_t max_index = 0;
    for (uint32_t i = 0; i < count; i++) {
      T v = xenos::GpuSwap(s[i], endian);
      d[i] = v;
      if (v > max_index) max_index = v;
    }
    return max_index;
  }

  // Returns the ring offset of `bytes` of guest data at `src` (byte-swapped by `endian`), copying only once per frame.
  uint32_t CopyBlockOnce(Frame& f, uint32_t guest, const uint8_t* src, uint32_t bytes, uint32_t endian, bool* fresh) {
    const uint64_t key = (uint64_t(guest) << 2) | (endian & 3);
    auto it = f.copied.find(key);
    if (it != f.copied.end() && it->second.bytes >= bytes && it->second.check == SampleCheck(src, it->second.bytes)) {
      if (fresh) *fresh = false;
      return it->second.off;
    }
    const uint32_t off = f.data.Alloc(bytes ? bytes : 16, 64);
    if (off == UINT32_MAX) return off;
    f.copied[key] = Frame::CopiedBlock{off, bytes, SampleCheck(src, bytes)};
    if (fresh) *fresh = true;
    return off;
  }

  bool CopyIndices(Frame& f, const IndexBufferInfo& ib, DkGpuAddr& addr_out, uint32_t& vertex_count_out) {
    uint32_t bytes = uint32_t(ib.length);
    {
      const bool i32 = ib.format == xenos::IndexFormat::kInt32;
      const uint32_t used = ib.count * (i32 ? 4 : 2);
      const uint8_t* src = memory_->TranslatePhysical(ib.guest_base);
      const uint64_t key = (uint64_t(ib.guest_base) << 32) ^ (uint64_t(ib.count) << 3) ^ (i32 ? 4 : 0) ^ uint32_t(ib.endianness);
      const auto endian = ib.endianness;
      const uint32_t count = ib.count;
      uint32_t max_index = 0;
      DkGpuAddr a = PersistLookup(persist_idx_, key, src, used,
                                  [i32, endian, count](uint8_t* d, const uint8_t* sp, uint32_t, uint32_t* mi) {
                                    *mi = i32 ? SwapIndices(reinterpret_cast<uint32_t*>(d), reinterpret_cast<const uint32_t*>(sp), count, endian)
                                              : SwapIndices(reinterpret_cast<uint16_t*>(d), reinterpret_cast<const uint16_t*>(sp), count, endian);
                                  },
                                  &max_index);
      if (a) {
        addr_out = a;
        vertex_count_out = max_index + 1;
        return true;
      }
    }
    uint32_t off = f.data.Alloc(bytes, 64);
    if (off == UINT32_MAX) { LogOnce("ring", "frame data ring full (indices)"); return false; }
    const uint8_t* src = memory_->TranslatePhysical(ib.guest_base);
    uint32_t max_index = 0;
    if (ib.format == xenos::IndexFormat::kInt32) {
      const uint32_t* s = reinterpret_cast<const uint32_t*>(src);
      uint32_t* d = reinterpret_cast<uint32_t*>(f.data.cpu + off);
      for (uint32_t i = 0; i < ib.count; i++) {
        uint32_t v = xenos::GpuSwap(s[i], ib.endianness);
        d[i] = v;
        if (v > max_index) max_index = v;
      }
    } else {
      const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
      uint16_t* d = reinterpret_cast<uint16_t*>(f.data.cpu + off);
      for (uint32_t i = 0; i < ib.count; i++) {
        uint16_t v = xenos::GpuSwap(s[i], ib.endianness);
        d[i] = v;
        if (v > max_index) max_index = v;
      }
    }
    addr_out = f.data.gpu + off;
    vertex_count_out = max_index + 1;
    return true;
  }

  // LEGO Batman port (perf, 2026-11-10): float constants are made finite and at most 2^60 in magnitude (NaN -> 0,
  // +-Inf and larger values -> +-2^60), the same bound the translator's FINITE_SPECIAL gives rcp/rsq/log/exp. Then
  // shaders may use plain multiplies wherever its taint analysis finds no Inf source: 0 * 2^60 is 0, as Xenos gives
  // for 0 * Inf. Without this, the fast shader set flickered on the console (a constant like FLT_MAX overflowing to
  // Inf in a plain multiply). Debug server "flag 15 1" turns it off (A/B).
  static inline uint32_t SanitizeFloats(uint32_t* w, uint32_t n) {
    uint32_t clamped = 0;
    for (uint32_t i = 0; i < n; i++) {
      const uint32_t v = w[i], e = (v >> 23) & 0xFF;
      if (e >= 188) {  // |v| >= 2^61, Inf or NaN
        w[i] = (e == 255 && (v & 0x7FFFFF)) ? 0u : ((v & 0x80000000u) | (187u << 23));
        clamped++;
      }
    }
    return clamped;
  }
  static inline uint32_t const_clamped_ = 0;
  static inline uint32_t const_late_ = 0;          // still extreme at upload (missed by the register-write clamp)
  static inline uint32_t const_late_reg_ = ~0u;    // the register of one such value (diagnostic)
  // The same clamp at upload, four floats at a time (NEON, no branches): the register-write clamp alone still let the
  // fast shader set flicker (a write path it does not see), the scalar version here cost ~10 ms of CPU per frame.
  static inline void SanitizeUpload(uint32_t* w, uint32_t n, uint32_t first_reg) {
#if defined(__aarch64__)
    const uint32x4_t k_ff = vdupq_n_u32(0xFF), k_188 = vdupq_n_u32(188), k_255 = vdupq_n_u32(255);
    const uint32x4_t k_man = vdupq_n_u32(0x7FFFFF), k_sign = vdupq_n_u32(0x80000000u), k_big = vdupq_n_u32(187u << 23);
    uint32x4_t late = vdupq_n_u32(0);
    uint32_t i = 0;
    for (; i + 4 <= n; i += 4) {
      uint32x4_t v = vld1q_u32(w + i);
      const uint32x4_t e = vandq_u32(vshrq_n_u32(v, 23), k_ff);
      const uint32x4_t big = vcgeq_u32(e, k_188);
      if (vmaxvq_u32(big) == 0) continue;
      const uint32x4_t nan = vandq_u32(vceqq_u32(e, k_255), vtstq_u32(v, k_man));
      const uint32x4_t repl = vbicq_u32(vorrq_u32(vandq_u32(v, k_sign), k_big), nan);
      vst1q_u32(w + i, vbslq_u32(big, repl, v));
      late = vsubq_u32(late, big);  // big lanes are all-ones: subtracting counts them
      const_late_reg_ = first_reg + i / 4;
    }
    const_late_ += vaddvq_u32(late);
    if (i < n) const_late_ += SanitizeFloats(w + i, n - i);
#else
    const uint32_t c = SanitizeFloats(w, n);
    if (c) const_late_reg_ = first_reg;
    const_late_ += c;
#endif
  }
  // Opt-in (debug server "flag 15 1"): only the fast shader set (alternative set 1) needs it; the exact default does not.
  static bool SanitizeOn() {
#ifndef CVHD_DK_GL
    return lbshot::flags[15] != 0;
#else
    return false;
#endif
  }

  static void CopyConstantSpan(uint8_t* dst, const uint32_t* src, const DkShaderObj* shader) {
    CopyConstantSpanRaw(dst, src, shader);
  }
  static void CopyConstantSpanRaw(uint8_t* dst, const uint32_t* src, const DkShaderObj* shader) {
    // LEGO Batman port (flicker hunt, 2026-11-10): the whole 4 KB block. With only the runs the SDK's analysis marks,
    // the rest of the block held the ring's bytes from two frames ago, which differ between the two frame slots: a
    // shader reading a constant the analysis missed got different values on even and odd frames. The console's
    // fingerprints showed the scene itself alternating while the copy chain matched.
#ifdef CVHD_DK_GL
    // Test on the GL host: LB_POISON_CONSTS=1 copies the marked runs only and fills the rest with NaN, to show whether
    // a shader reads a constant the analysis missed.
    static const bool poison = std::getenv("LB_POISON_CONSTS") != nullptr;
    if (poison && shader) std::memset(dst, 0xFF, 4096);
    if (!poison)
#endif
    // The upload-time check found nothing the register-write clamp missed in a whole intro + gameplay run (0 values)
    // and cost ~10 ms of CPU per frame: debug server "flag 16 1" only (diagnostic).
#ifndef CVHD_DK_GL
    const bool clean = SanitizeOn() && lbshot::flags[16];
#else
    const bool clean = false;
#endif
    if (!shader) {
      std::memcpy(dst, src, 4096);
      if (clean) SanitizeUpload(reinterpret_cast<uint32_t*>(dst), 1024, 0);
      return;
    }
    // Each run of consecutive registers the shader reads (the sprite pixel shader reads c3-c4 and c254-c255: one
    // span from the lowest to the highest was still 4 KB).
    const auto& map = shader->constant_register_map();
    int run = -1;
    for (int i = 0; i <= 256; i++) {
      const bool used = i < 256 && (map.float_bitmap[i >> 6] >> (i & 63)) & 1;
      if (used && run < 0) run = i;
      if (!used && run >= 0) {
        std::memcpy(dst + run * 16, src + run * 4, size_t(i - run) * 16);
        if (clean) SanitizeUpload(reinterpret_cast<uint32_t*>(dst + run * 16), uint32_t(i - run) * 4, uint32_t(run));
        run = -1;
      }
    }
  }

  bool BindUniforms(Frame& f, DkShaderObj* vs, DkShaderObj* ps, const draw_util::ViewportInfo& vi, const RegisterFile& regs) {
    DkCmdBuf cmd = f.cmdbuf;
    // A block is copied again only when its registers were written since (WriteRegister) or another shader reads
    // another span of them; only the runs of float constants the shader reads are copied (dynamic indexing sets
    // every bit). The deko3d binding is sticky within the frame's command buffer: rebind on a new block only.
    bool bind_vs = false, bind_ps = false;
    if (vs_consts_dirty_ || f.vs_uni == UINT32_MAX || f.vs_uni_shader != vs) {
      const uint32_t off = f.data.Alloc(4096, DK_UNIFORM_BUF_ALIGNMENT);
      if (off == UINT32_MAX) { LogOnce("ring", "frame data ring full (uniforms)"); return false; }
      CopyConstantSpan(f.data.cpu + off, &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], vs);
      f.vs_uni = off;
      f.vs_uni_shader = vs;
      vs_consts_dirty_ = false;
      bind_vs = true;
      uni_uploads_[0]++;
    }
    if (ps_consts_dirty_ || f.ps_uni == UINT32_MAX || f.ps_uni_shader != ps) {
      const uint32_t off = f.data.Alloc(4096, DK_UNIFORM_BUF_ALIGNMENT);
      if (off == UINT32_MAX) { LogOnce("ring", "frame data ring full (uniforms)"); return false; }
      CopyConstantSpan(f.data.cpu + off, &regs.values[XE_GPU_REG_SHADER_CONSTANT_256_X], ps);
      f.ps_uni = off;
      f.ps_uni_shader = ps;
      ps_consts_dirty_ = false;
      bind_ps = true;
      uni_uploads_[1]++;
    }
    SysConstants sys{};
    for (int i = 0; i < 3; i++) {
      sys.ndc_scale[i] = vi.ndc_scale[i];
      sys.ndc_offset[i] = vi.ndc_offset[i];
    }
    auto cc = regs.Get<reg::RB_COLORCONTROL>();
    sys.alpha[0] = cc.alpha_test_enable ? 1.0f : 0.0f;
    sys.alpha[1] = float(uint32_t(cc.alpha_func));
    sys.alpha[2] = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
    sys.alpha[3] = 0.0f;  // was the DBG TEX LOD switch (0.258-0.276); the fetches use the computed LOD now
    std::memcpy(sys.bools, &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], sizeof(sys.bools));
    std::memcpy(sys.loops, &regs.values[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], sizeof(sys.loops));
    if (f.sys_uni == UINT32_MAX || std::memcmp(&sys, &f.last_sys, sizeof(sys)) != 0) {
      const uint32_t off = f.data.Alloc(sizeof(SysConstants), DK_UNIFORM_BUF_ALIGNMENT);
      if (off == UINT32_MAX) { LogOnce("ring", "frame data ring full (uniforms)"); return false; }
      std::memcpy(f.data.cpu + off, &sys, sizeof(sys));
      f.sys_uni = off;
      f.last_sys = sys;
      bind_vs = bind_ps = true;
      uni_uploads_[2]++;
    }
    if (vs->generic && f.vfetch_shader != vs) {
      const uint32_t off = f.data.Alloc(sizeof(vs->gen_swz), DK_UNIFORM_BUF_ALIGNMENT);
      if (off == UINT32_MAX) { LogOnce("ring", "frame data ring full (uniforms)"); return false; }
      std::memcpy(f.data.cpu + off, vs->gen_swz, sizeof(vs->gen_swz));
      dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 2, f.data.gpu + off, sizeof(vs->gen_swz));
      f.vfetch_shader = vs;
    }
    if (bind_vs) {
      DkBufExtents vs_bufs[2] = {{f.data.gpu + f.vs_uni, 4096}, {f.data.gpu + f.sys_uni, sizeof(SysConstants)}};
      dkCmdBufBindUniformBuffers(cmd, DkStage_Vertex, 0, vs_bufs, 2);
    }
    if (bind_ps) {
      DkBufExtents ps_bufs[2] = {{f.data.gpu + f.ps_uni, 4096}, {f.data.gpu + f.sys_uni, sizeof(SysConstants)}};
      dkCmdBufBindUniformBuffers(cmd, DkStage_Fragment, 0, ps_bufs, 2);
    }
    return true;
  }

  // The XDK's Clear: rectangles (position xyz + color rgba, stride 7 words) with the depth
  // function "always"; half of them through the 2x MSAA half-pitch alias of the surface.
  bool IsClearDraw(DkShaderObj* vs, DkShaderObj* ps, xenos::PrimitiveType prim) {
    if (prim != xenos::PrimitiveType::kRectangleList || !ps->texture_bindings().empty()) return false;
    const auto& vbs = vs->vertex_bindings();
    if (vbs.size() != 1 || vbs[0].stride_words != 7 || vbs[0].attributes.size() != 2) return false;
    const auto& a0 = vbs[0].attributes[0].fetch_instr.attributes;
    const auto& a1 = vbs[0].attributes[1].fetch_instr.attributes;
    return a0.data_format == xenos::VertexFormat::k_32_32_32_FLOAT && a0.offset == 0 &&
           a1.data_format == xenos::VertexFormat::k_32_32_32_32_FLOAT && a1.offset == 3;
  }

  bool ClearDraw(uint32_t vertex_count, xenos::EdramMode mode) {
    const RegisterFile& regs = *register_file_;
    Frame& f = frames_[frame_];
    DkCmdBuf cmd = f.cmdbuf;
    FlushResolveBarrier(cmd);
    GpuScope gs(this, f, kTagClear);
    StateCacheReset();  // deko3d clears/copies may change bound state
    PsMark(f, 2);
    auto fetch = regs.GetVertexFetch(0);
    const uint32_t* words = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(fetch.address << 2));
    auto surf = regs.Get<reg::RB_SURFACE_INFO>();
    auto color_info = regs.Get<reg::RB_COLOR_INFO>();
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    uint32_t scale = surf.msaa_samples == xenos::MsaaSamples::k4X ? 2 : 1;
    uint32_t pitch = EffectivePitch(surf);
    auto word = [&](uint32_t i) {
      uint32_t w = words[i];
      if (fetch.endian == xenos::Endian::k8in32) w = __builtin_bswap32(w);
      float v;
      std::memcpy(&v, &w, 4);
      return v;
    };
    for (uint32_t r = 0; r + 3 <= vertex_count && r * 7 + 21 <= fetch.size; r += 3) {
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      for (uint32_t v = 0; v < 3; v++) {
        float x = word((r + v) * 7), y = word((r + v) * 7 + 1);
        x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
      }
      float z = word(r * 7 + 2);
      float color[4] = {word(r * 7 + 3), word(r * 7 + 4), word(r * 7 + 5), word(r * 7 + 6)};
      DkScissor sc;
      sc.x = uint32_t(std::max(0.0f, std::floor(x0 + 0.5f))) * scale;
      sc.y = uint32_t(std::max(0.0f, std::floor(y0 + 0.5f))) * scale;
      sc.width = uint32_t(std::max(0.0f, std::floor(x1 + 0.5f) - std::floor(x0 + 0.5f))) * scale;
      sc.height = uint32_t(std::max(0.0f, std::floor(y1 + 0.5f) - std::floor(y0 + 0.5f))) * scale;
      HostImage* color_img = nullptr;
      HostImage* depth_img = nullptr;
      if (mode == xenos::EdramMode::kColorDepth) {
        color_img = TargetImage(color_info.color_base, pitch, ColorTargetFormatOf(color_info.color_format), false);
        depth_img = TargetImage(depth_info.depth_base, pitch, DkImageFormat_Z24S8, true);
      } else if (mode == xenos::EdramMode::kDepthOnly) {
        // Depth-only through the alias: the XDK fills a color surface's EDRAM this way too.
        auto it = targets_.find((uint64_t(depth_info.depth_base) << 32) | (uint64_t(pitch) << 1));
        if (it != targets_.end()) color_img = it->second;
        else depth_img = TargetImage(depth_info.depth_base, pitch, DkImageFormat_Z24S8, true);
      }
      if (!color_img && !depth_img) continue;
      BindTargets(cmd, color_img, depth_img);
      const HostImage* cimg = color_img ? color_img : depth_img;
      uint32_t w = cimg->gw, h = cimg->gh;  // guest pixels
      if (sc.x >= w || sc.y >= h) continue;
      if (sc.x + sc.width > w) sc.width = w - sc.x;
      if (sc.y + sc.height > h) sc.height = h - sc.y;
      const DkScissor key_sc = sc;
      if (cimg->hs != kScaleDen) sc = ScaleRect(sc.x, sc.y, sc.width, sc.height, cimg);
      dkCmdBufSetScissors(cmd, 0, &sc, 1);
      if (color_img) dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, color);
      if (depth_img) dkCmdBufClearDepthStencil(cmd, true, z, 0xFF, 0);
      {
        char key[96];
        std::snprintf(key, sizeof(key), "clear %u %u %u %u %u %u %u %u", uint32_t(mode), color_info.color_base, depth_info.depth_base, pitch,
                      key_sc.x, key_sc.y, key_sc.width, key_sc.height);
        LogOnce(key, "clear %s%s at EDRAM %u/%u pitch %u: rect %u,%u %ux%u color %g %g %g %g z %g (alias x%u)", color_img ? "color" : "",
                depth_img ? " depth" : "", color_info.color_base, depth_info.depth_base, pitch, sc.x, sc.y, sc.width, sc.height, color[0],
                color[1], color[2], color[3], z, scale);
      }
      clears_++;
      frame_clears_++;
    }
    return true;
  }

  bool Resolve() {
    const RegisterFile& regs = *register_file_;
    Frame& f = frames_[frame_];
    DkCmdBuf cmd = f.cmdbuf;
    GpuScope gs(this, f, kTagResolve);
    PsMark(f, 1);
    StateCacheReset();  // deko3d clears/copies may change bound state
    draw_util::ResolveInfo info;
    if (!draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) { LogOnce("resolve", "GetResolveInfo failed"); return true; }
    if (info.IsCopyingDepth()) {
      LogOnce("depthresolve", "depth resolve to %08X ignored", info.copy_dest_base);
      return true;
    }
    uint32_t src_pitch = info.color_edram_info.pitch_tiles * 80;
    HostImage* src = TargetImage(info.color_edram_info.base_tiles, src_pitch, DkImageFormat_RGBA8_Unorm, false);
    if (!src) return true;
    uint32_t x = info.coordinate_info.edram_offset_x_div_8 * 8, y = info.coordinate_info.edram_offset_y_div_8 * 8;
    uint32_t w = info.coordinate_info.width_div_8 * 8, h = info.height_div_8 * 8;
    uint32_t dest_pitch = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
    uint32_t dest_height = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
    (void)dest_pitch; (void)dest_height;
    uint32_t dx = info.copy_dest_coordinate_info.offset_x_div_8 * 8, dy = info.copy_dest_coordinate_info.offset_y_div_8 * 8;
    // LEGO Batman port: the game renders 1280x720 in two EDRAM passes (1280x512, then 1280x208) and resolves the
    // second one to the address of row 512 of the same tiled surface (base + 512 * pitch * 4: a 32-row-aligned
    // row of a tiled 32 bpp surface starts there). Resolve it into that surface's image at that row, so the
    // full-screen fetch of the surface sees both parts instead of 512 rows plus cleared ones.
    uint32_t dest_base = info.copy_dest_base;
    uint32_t merged_row = 0;
    if (dest_pitch && !guest_images_.count(dest_base)) {
      const uint32_t row_bytes = dest_pitch * 4;
      for (auto& it : guest_images_) {
        HostImage* im = it.second;
        if (!im->from_resolve || im->format != DkImageFormat_RGBA8_Unorm || im->gw != dest_pitch || it.first >= dest_base) continue;
        const uint32_t delta = dest_base - it.first;
        if (delta % (row_bytes * 32)) continue;
        const uint32_t row = delta / row_bytes;
        if (row > im->gh) continue;  // not adjacent to (or inside) that image
        char key[48];
        std::snprintf(key, sizeof(key), "merge %08X %08X", it.first, dest_base);
        LogOnce(key, "resolve to %08X = row %u of the resolve image at %08X: merged", dest_base, row, it.first);
        dest_base = it.first;
        dy += row;
        merged_row = row;
        break;
      }
    }
    // Single-pass tiles: the bottom tile was drawn at its final rows, so its resolve reads them there.
    if (merged_row && SinglePass() && frame_offset_draws_ && y < merged_row) y += merged_row;
    HostImage* dst = GuestImage(f, dest_base, dx + w, dy + h, DkImageFormat_RGBA8_Unorm, true, 0, 0, src->hs);  // gh: exact size (the aligned one made it ping-pong with the texture bind)
    if (!dst) return true;
    dst->from_resolve = true;
    // Single-pass tiles: a resolve of one tile's rows. A draw that samples this image later in the frame depends on the
    // tile being drawn (the game's per-tile post effect), so it is not drawn once for the whole picture (IssueDrawImpl).
    if (tile_h_ && (frame_offset_draws_ || prev_frame_tiled_) && h <= tile_h_ && h < dst->gh) {
      tile_post_ = true;  // the rest of this tile is its post pass: drawn per tile
      if (dst->tile_resolved != swaps_ + 1) dst->tile_rows = 0;
      dst->tile_resolved = swaps_ + 1;
      dst->tile_rows = std::max(dst->tile_rows, dy + h);
    }
    if (FrameLogOn())
      Log("frm #%u resolve -> %08X (guest %08X) img %05X %ux%u at %u,%u rect %ux%u", swaps_ + 1, dest_base, info.copy_dest_base,
          ImgId(dst), dst->width, dst->height, dx, dy, w, h);
    dst->content_serial = serial_;
    dst->mem_bytes = std::max(dest_pitch * dest_height, dest_pitch * ((dst->gh + 31) & ~31u)) * 4;
#ifdef CVHD_DK_GL
    dst->mem_hash = HashMemory(dest_base, dst->mem_bytes);
#endif
    // LEGO Batman port (perf): the hash is not taken on the console: MemoryChanged never compares it for a resolve
    // destination (from_resolve), and sampling ~3.6 MB per resolve x 18 resolves a frame showed in the GPU thread's
    // profile (~10%).
    // LEGO Batman port: no page watch on resolve destinations. On the Switch a watch unmaps the pages (a mapping
    // change under the global lock); re-arming ~6 multi-MB resolve targets per frame showed up as svcMap/Unmap and
    // lock waits on the GPU thread, and the game hardly ever writes them from the CPU.
    if (x + w > src->gw) w = x < src->gw ? src->gw - x : 0;
    if (y + h > src->gh) h = y < src->gh ? src->gh - y : 0;
    if (dx + w > dst->gw) w = dx < dst->gw ? dst->gw - dx : 0;
    if (dy + h > dst->gh) h = dy < dst->gh ? dst->gh - dy : 0;
    // LEGO Batman port (perf): the same resolve again with nothing drawn, cleared or uploaded since (the game
    // resolves its final picture to the front buffer four times in a row) copies the same bytes: skipped. Debug server
    // "flag 13 1" copies every time (A/B).
    const uint64_t rsig[5] = {uint64_t(reinterpret_cast<uintptr_t>(src)), uint64_t(reinterpret_cast<uintptr_t>(dst)),
                              (uint64_t(x) << 32) | y, (uint64_t(w) << 32) | h, (uint64_t(dx) << 32) | dy};
    const uint64_t rwork = draws_ + clears_ + uploads_ + resolve_clears_;
    bool same_resolve = std::memcmp(rsig, last_resolve_sig_, sizeof(rsig)) == 0 && rwork == last_resolve_work_;
#ifndef CVHD_DK_GL
    if (lbshot::flags[13]) same_resolve = false;
#endif
    std::memcpy(last_resolve_sig_, rsig, sizeof(rsig));
    last_resolve_work_ = rwork;
    if (same_resolve) {
      resolves_skipped_++;
    } else if (w && h) {
      DkImageView sv, dv;
      dkImageViewDefaults(&sv, &src->image);
      dkImageViewDefaults(&dv, &dst->image);
      // Host pixels: both images have the same scale (GuestImage remakes a destination of another one).
      const DkScissor hsr = ScaleRect(x, y, w, h, src), hdr = ScaleRect(dx, dy, w, h, dst);
      const uint32_t cw = std::min(hsr.width, hdr.width), ch = std::min(hsr.height, hdr.height);
      DkImageRect sr{hsr.x, hsr.y, 0, cw, ch, 1}, dr{hdr.x, hdr.y, 0, cw, ch, 1};
      // The barrier before the copy also completes a deferred one of the previous resolve (identical barriers).
      resolve_barrier_pending_ = false;
      dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
      dkCmdBufCopyImage(cmd, &sv, &sr, &dv, &dr, 0);
      // Deferred: back-to-back resolves (the MRTs after each tile) share one barrier; FlushResolveBarrier() emits it
      // before any other GPU work.
      resolve_barrier_pending_ = true;
#ifndef CVHD_DK_GL
      if (lbshot::flags[13]) FlushResolveBarrier(cmd);
#endif
    }
    if (info.IsClearingColor()) {
      FlushResolveBarrier(cmd);  // the copy must finish reading the source before it is cleared
      resolve_clears_++;
      BindTargets(cmd, src, nullptr);
      DkScissor sc = ScaleRect(x, y, w, h, src);
      dkCmdBufSetScissors(cmd, 0, &sc, 1);
      uint32_t c = info.rb_color_clear;
      float color[4] = {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f};
      dkCmdBufClearColor(cmd, 0, DkColorMask_RGBA, color);
    }
    {
      // Every distinct resolve (source rect, destination and offset) once: the frame structure.
      char key[96];
      std::snprintf(key, sizeof(key), "resolve %u %u %u %u %u %u %08X %u %u", info.color_edram_info.base_tiles, src_pitch, x, y, w, h,
                    info.copy_dest_base, dx, dy);
      LogOnce(key, "resolve EDRAM %u pitch %u rect %u,%u %ux%u -> %08X (%ux%u image) at %u,%u%s", info.color_edram_info.base_tiles, src_pitch,
              x, y, w, h, info.copy_dest_base, dst->width, dst->height, dx, dy, info.IsClearingColor() ? ", then cleared" : "");
    }
    resolves_++;
    frame_resolves_++;
    return true;
  }

  // ----- shaders --------------------------------------------------------------------------------
  // The same two files as Shader::DumpUcode (shader_<hash>.ucode.bin.<ext> and the disassembly
  // shader_<hash>.ucode.<ext>, the translator's input), written with stdio: the SDK's version goes
  // through std::filesystem::absolute, which turns sdmc:/x into /sdmc:/x on newlib (finding 0.41).
  void DumpMissing(const DkShaderObj& shader) {
    std::string dir = shader_dir_ + "missing/";
    mkdir(shader_dir_.c_str(), 0777);  // LEGO Batman port: the shader folder itself may be absent on the SD
    mkdir(dir.c_str(), 0777);
    const char* ext = shader.type() == xenos::ShaderType::kVertex ? "vert" : "frag";
    char name[512];
    std::snprintf(name, sizeof(name), "%sshader_%016llX.ucode.bin.%s", dir.c_str(), (unsigned long long)shader.ucode_data_hash(), ext);
    if (FILE* fp = std::fopen(name, "wb")) {
      std::fwrite(shader.ucode_data().data(), 4, shader.ucode_data().size(), fp);
      std::fclose(fp);
    } else {
      Log("cannot write %s", name);
    }
    std::snprintf(name, sizeof(name), "%sshader_%016llX.ucode.%s", dir.c_str(), (unsigned long long)shader.ucode_data_hash(), ext);
    if (FILE* fp = std::fopen(name, "w")) {
      std::fwrite(shader.ucode_disassembly().data(), 1, shader.ucode_disassembly().size(), fp);
      std::fclose(fp);
    }
  }

  bool LoadDksh(const std::string& path, DkShader& out) {
#ifdef CVHD_DK_GL
    // Linux: the translated GLSL next to where the DKSH would be (name.dksh -> name.glsl).
    std::string glsl = path.size() > 5 && path.compare(path.size() - 5, 5, ".dksh") == 0 ? path.substr(0, path.size() - 5) + ".glsl" : path;
    FILE* gp = std::fopen(glsl.c_str(), "rb");
    if (!gp) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), gp)) > 0) text.append(buf, n);
    std::fclose(gp);
    DkStage stage = path.find(".vert") != std::string::npos ? DkStage_Vertex : path.find(".geom") != std::string::npos ? DkStage_Geometry : DkStage_Fragment;
    dkShaderInitializeGlsl(&out, stage, text.c_str());
    return true;
#else
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    struct { uint32_t magic, header_sz, control_sz, code_sz, programs_off, num_programs; } hdr;
    if (std::fread(&hdr, sizeof(hdr), 1, fp) != 1 || hdr.control_sz < sizeof(hdr) || hdr.control_sz > (1u << 20) || hdr.code_sz > (4u << 20)) {
      std::fclose(fp);
      return false;
    }
    std::vector<uint8_t> control(hdr.control_sz);
    std::rewind(fp);
    if (std::fread(control.data(), hdr.control_sz, 1, fp) != 1) { std::fclose(fp); return false; }
    uint32_t off = code_.Alloc(hdr.code_sz, DK_SHADER_CODE_ALIGNMENT);
    if (off == UINT32_MAX) { std::fclose(fp); Log("shader code pool full"); return false; }
    if (std::fread(code_.cpu + off, hdr.code_sz, 1, fp) != 1) { std::fclose(fp); return false; }
    std::fclose(fp);
    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, code_.block, off);
    sm.control = control.data();
    sm.programId = 0;
    dkShaderInitialize(&out, &sm);
    return true;
#endif
  }

  // First sighting of a numeric key (the per-draw diagnostics: no string building on the command thread).
  bool FirstTime(uint64_t key) { return seen_.insert(key).second; }
  void LogOnce(const char* key, const char* fmt, ...) {
    if (!logged_.insert(key).second) return;
    va_list ap;
    va_start(ap, fmt);
    std::printf("[dk] ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    std::fflush(stdout);
  }

  DkDevice device_ = nullptr;
  DkQueue queue_ = nullptr;
  DkSwapchain swapchain_ = nullptr;
  MemBlock code_, images_, descriptors_, cmdmem_;
  uint32_t sampler_desc_offset_ = 0;
  Frame frames_[kFrames];
  uint32_t frame_ = 0;
  HostImage* swap_images_[kFrames] = {};
  DkImage const* swap_image_ptrs_[kFrames] = {};
  DkShader present_vs_shader_{}, present_fs_shader_{};
  bool present_vs_ = false, present_fs_ = false;
  bool present_target_ = false;
#ifndef CVHD_DK_GL
  // Port menu RECORD 5 SEC (nx_menu.cpp, switch/host/frame_recorder.cpp, from hcu-switch): the 1920x1080 guest front buffer of each
  // drawn frame is copied into rec_ (one slot per frame in flight) and handed to the recorder once the frame's fence has
  // passed (BeginFrame): the command thread never waits for it. The file sdmc:/switch/cvhd/record_now does the same
  // (scripted runs; removed when seen, checked with the fps mode every 60 swaps).
  static constexpr uint32_t kRecW = 1280, kRecH = 720;  // LEGO Batman port: its front buffer (cvhd's is 1920x1080)
  // LEGO Batman port: GpuUncached (was GpuCached): the GPU's copy sat in its L2 when the CPU read the slot, so the
  // recorder sometimes saved the frame from two recordings earlier (an even/odd interleave that the screen never showed).
  MemBlock rec_;
  bool rec_on_ = false;
  uint64_t rec_start_tick_ = 0;
  void RecordFrame(Frame& f, DkCmdBuf cmd, HostImage* shown) {
    FlushResolveBarrier(cmd);
    if (!rec_on_ && rex_nx_debug_record()) {
      if (!rec_.block && !rec_.Create(device_, kRecW * kRecH * 4 * kFrames, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuUncached)) {
        Log("RECORD 5 SEC: no memory block");
        rex_nx_debug_record_set(0);
        return;
      }
      if (!rex_rec_start((debug_dir_ + "rec/").c_str(), swaps_ + 1, kRecW, kRecH)) {
        rex_nx_debug_record_set(0);
        return;
      }
      rec_on_ = true;
      rec_start_tick_ = armGetSystemTick();
    }
    if (!rec_on_) return;
    const uint64_t us = armTicksToNs(armGetSystemTick() - rec_start_tick_) / 1000;
    if (us > 5000000) {
      // Done: the frames still in flight are handed over, then the writer finishes the file.
      dkQueueWaitIdle(queue_);
      const uint32_t bytes = kRecW * kRecH * 4;
      for (uint32_t i = 0; i < kFrames; i++) {
        if (!frames_[i].rec_pending) continue;
        dkMemBlockFlushCpuCache(rec_.block, i * bytes, bytes);
        rex_rec_push(rec_.cpu + i * bytes, frames_[i].rec_swap, frames_[i].rec_us);
        frames_[i].rec_pending = false;
      }
      rex_rec_stop();
      rec_on_ = false;
      rex_nx_debug_record_set(0);
      return;
    }
    if (!shown || shown->width != kRecW || shown->height != kRecH || shown->format != DkImageFormat_RGBA8_Unorm) return;
    DkImageView v;
    dkImageViewDefaults(&v, &shown->image);
    DkImageRect r{0, 0, 0, kRecW, kRecH, 1};
    DkCopyBuf cb{rec_.gpu + uint64_t(frame_) * kRecW * kRecH * 4, 0, 0};
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    dkCmdBufCopyImageToBuffer(cmd, &v, &r, &cb, 0);
    f.rec_pending = true;
    f.rec_swap = swaps_ + 1;
    f.rec_us = us;
  }
#endif
  uint32_t swap_interval_ = 1;    // ReadFpsMode: vblanks per presented frame
  double shown_last_ms_ = 0;      // the last present (the shown-frame intervals of the dk time/s line)
  std::vector<double> shown_ms_;
  bool dump_images_ = false;
  bool trace_enabled_ = false, traced_ = false, trace_any_ = false;  // file dk_trace: one line per draw of the first frame with 440+ draws
  uint32_t trace_min_swap_ = 0;  // seconds after the backend started
  double trace_start_ms_ = 0;
  uint32_t trace_second_ = 0;
  uint32_t ring_full_swap_ = 0;  // swap of the first staging-ring-full event (the first stage frame)
  uint32_t trace_after_full_ = 60;
  uint32_t frame_sprite_draws_ = 0;  // draws of the paletted sprite pixel shader in this frame
  bool frame_big_tex_ = false;  // a stage-sized (4000+ wide) texture was fetched in this frame
  bool frame_mip_only_ = false, trace_on_mip_ = false;  // a texture without its base level was fetched in this frame
  bool traced_after_ = false;
  std::string debug_dir_;
  std::vector<std::string> trace_lines_;
  std::string shader_dir_;
  struct ShaderQuick { Shader* shader = nullptr; uint32_t probe[8] = {}; };
  std::unordered_map<uint64_t, ShaderQuick> shader_quick_;
  std::string load_dir_;  // where the DKSH files are read: romfs:/shaders/ when the NRO carries them, else shader_dir_
  std::vector<uint32_t> page_serial_ = std::vector<uint32_t>(0x20000);  // per 4 KB physical page: the draw serial of its last write
  std::vector<uint8_t> page_seen_ = std::vector<uint8_t>(0x20000);  // in frame_pages_ already
  std::vector<uint32_t> frame_pages_, last_frame_pages_;            // pages seen dirty in this / the previous frame
  uint8_t recent_coarse_[0x200] = {};                               // 1 MB regions seen dirty lately (frames left)
  uint32_t frame_start_serial_ = 0;
  uint32_t serial_ = 1;  // the draw serial (SyncDirtyPages)
  uint64_t dirty_pages_ = 0, dirty_pages_at_swap_ = 0;
  std::unordered_map<uint64_t, DkShaderObj*> shaders_;
  std::unordered_map<uint64_t, HostImage*> targets_;       // EDRAM base/pitch -> render target
  std::unordered_map<uint32_t, HostImage*> guest_images_;  // guest address -> texture / resolve destination
  HostImage* bound_color_ = nullptr;
  HostImage* bound_depth_ = nullptr;
  std::unordered_set<std::string> logged_;
  std::unordered_set<uint64_t> seen_;
  uint32_t swaps_ = 0, draws_ = 0, skipped_ = 0, missing_draws_ = 0, missing_shaders_ = 0, clears_ = 0, resolves_ = 0;
  bool prefer_generic_vs_ = false;
  uint32_t generic_shaders_ = 0;  // LEGO Batman port: vertex shaders drawn with their generic (disc) translation
  std::chrono::steady_clock::time_point hud_window_start_{};  // the fps counter's one-second window
  uint32_t hud_window_swaps_ = 0;
  double t_draw_ms_ = 0, t_acquire_ms_ = 0, t_fence_ms_ = 0;  // see IssueDraw
  double t_phase_ms_[7] = {};  // IssueDrawImpl phases
  double t_gpu_ms_ = 0;  // GPU time between the frame's timestamps
  uint32_t gpu_frames_ = 0;
  uint32_t uni_uploads_[3] = {};  // BindUniforms copies per second: vs, ps, sys
  uint32_t texbind_hits_ = 0;  // texture bindings served by the frame's cache
  bool vs_consts_dirty_ = true, ps_consts_dirty_ = true;  // WriteRegister
  char hud_text_[9] = "";  // what the present pass draws top-left (fps, one decimal)
  uint32_t image_count_ = 0, uploads_ = 0, reuploads_ = 0;
  uint64_t upload_bytes_ = 0;
  uint32_t frame_draws_ = 0, frame_clears_ = 0, frame_resolves_ = 0, frame_skipped_ = 0, frame_missing_ = 0, frame_reuploads_ = 0;
  struct Uploaded { uint32_t base, bytes, serial; };  // by value: a recycled image may change hands within the frame
  std::vector<Uploaded> frame_uploaded_;  // uploads of this frame (cleared at the swap)
  uint32_t late_writes_ = 0;               // per second: uploaded textures written again before their frame's swap
};

class Deko3dGraphicsSystem : public GraphicsSystem {
 public:
  std::string name() const override { return "deko3d"; }
  // The backend owns the default window's swapchain; the SDK keeps no presenter.
  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext*) override { return X_STATUS_SUCCESS; }

 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<Deko3dCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace

rex::system::IGraphicsSystem* CreateDeko3dGraphicsSystem() { return new Deko3dGraphicsSystem(); }

}  // namespace rex::graphics::deko
#endif  // __SWITCH__ || CVHD_DK_GL
