// Trace GPU backend (--gpu_backend=trace): the SDK's Xenos command processor runs as usual (ring
// buffer, fences, read-pointer write-back, vblank, bin masks), nothing is drawn, and every distinct
// draw / resolve / swap state is logged once with the registers a native renderer consumes. It is
// the state table for the deko3d backend (docs/native-renderer-plan.md, M3a) and runs on Linux and
// in Eden. cvhd-recomp, BSD-3 like the SDK. Compiled into the SDK's GPU plugin through
// REXGPU_XENOS_EXTRA_SOURCES (switch/sdk-edits.py adds the hook rex_gpu_extra_backend).
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/memory.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/gpu_plugin.h>

#include "ucode_hash.h"

namespace rex::graphics::trace {

namespace {

const char* PrimName(xenos::PrimitiveType t) {
  switch (t) {
    case xenos::PrimitiveType::kPointList: return "points";
    case xenos::PrimitiveType::kLineList: return "lines";
    case xenos::PrimitiveType::kLineStrip: return "linestrip";
    case xenos::PrimitiveType::kTriangleList: return "tris";
    case xenos::PrimitiveType::kTriangleFan: return "trifan";
    case xenos::PrimitiveType::kTriangleStrip: return "tristrip";
    case xenos::PrimitiveType::kRectangleList: return "rects";
    case xenos::PrimitiveType::kQuadList: return "quads";
    case xenos::PrimitiveType::kQuadStrip: return "quadstrip";
    default: return "prim?";
  }
}

const char* VertexFormatName(xenos::VertexFormat f) {
  switch (f) {
    case xenos::VertexFormat::k_8_8_8_8: return "8888";
    case xenos::VertexFormat::k_2_10_10_10: return "2101010";
    case xenos::VertexFormat::k_16_16: return "1616";
    case xenos::VertexFormat::k_16_16_16_16: return "16161616";
    case xenos::VertexFormat::k_16_16_FLOAT: return "1616f";
    case xenos::VertexFormat::k_16_16_16_16_FLOAT: return "16161616f";
    case xenos::VertexFormat::k_32: return "32";
    case xenos::VertexFormat::k_32_32: return "3232";
    case xenos::VertexFormat::k_32_32_32_32: return "32323232";
    case xenos::VertexFormat::k_32_FLOAT: return "32f";
    case xenos::VertexFormat::k_32_32_FLOAT: return "3232f";
    case xenos::VertexFormat::k_32_32_32_FLOAT: return "323232f";
    case xenos::VertexFormat::k_32_32_32_32_FLOAT: return "32323232f";
    default: return "vfmt?";
  }
}

// 64-bit FNV-1a of the state text: the dedupe key (the SDK's XXH3 is not needed here).
uint64_t Fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

class TraceCommandProcessor : public CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;

 protected:
  bool SetupContext() override {
    REXLOG_INFO("[trace] backend up: every distinct draw/resolve/swap state is logged once");
    return true;
  }
  void ShutdownContext() override {
    for (auto& it : shaders_) delete it.second;
    shaders_.clear();
  }

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override {
    // Hash as the Vulkan backend hashes (XXH3 of the dwords) so the dump names match its dumps.
    uint64_t hash = ShaderHash(host_address, dword_count);
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) return it->second;
    Shader* shader = new Shader(shader_type, hash, host_address, dword_count);
    string::StringBuffer disasm;
    shader->AnalyzeUcode(disasm);
    shaders_.emplace(hash, shader);
    const std::string& dump_dir = REXCVAR_GET(dump_shaders);
    if (!dump_dir.empty()) shader->DumpUcode(dump_dir);
    std::string bindings;
    if (shader_type == xenos::ShaderType::kVertex) {
      for (const auto& vb : shader->vertex_bindings()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), " vf%u(stride %u:", vb.fetch_constant, vb.stride_words);
        bindings += buf;
        for (const auto& a : vb.attributes) {
          std::snprintf(buf, sizeof(buf), " %s@%d", VertexFormatName(a.fetch_instr.attributes.data_format),
                        a.fetch_instr.attributes.offset);
          bindings += buf;
        }
        bindings += ")";
      }
    } else {
      for (const auto& tb : shader->texture_bindings()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " tf%u", tb.fetch_constant);
        bindings += buf;
      }
    }
    REXLOG_INFO("[trace] shader {} {:016X} at {:08X} {} dwords, bindings:{}",
                shader_type == xenos::ShaderType::kVertex ? "VS" : "PS", hash, guest_address,
                dword_count, bindings);
    return shader;
  }

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override {
    draws_++;
    frame_draws_++;
    const RegisterFile& regs = *register_file_;
    Shader* vs = active_vertex_shader();
    Shader* ps = active_pixel_shader();
    char buf[256];
    std::string s;
    std::snprintf(buf, sizeof(buf), "%s n=%u ib=%s vs=%016llX ps=%016llX", PrimName(prim_type), index_count,
                  index_buffer_info ? "yes" : "auto", vs ? (unsigned long long)vs->ucode_data_hash() : 0ull,
                  ps ? (unsigned long long)ps->ucode_data_hash() : 0ull);
    s += buf;
    if (index_buffer_info) {
      std::snprintf(buf, sizeof(buf), " (fmt %u endian %u base %08X len %zu)",
                    uint32_t(index_buffer_info->format), uint32_t(index_buffer_info->endianness),
                    index_buffer_info->guest_base, index_buffer_info->length);
      s += buf;
    }
    auto surf = regs.Get<reg::RB_SURFACE_INFO>();
    auto color = regs.Get<reg::RB_COLOR_INFO>();
    auto depth = regs.Get<reg::RB_DEPTH_INFO>();
    auto mode = regs.Get<reg::RB_MODECONTROL>();
    std::snprintf(buf, sizeof(buf), " | rt pitch %u msaa %u color base %u fmt %u depth base %u fmt %u edram_mode %u",
                  surf.surface_pitch, uint32_t(surf.msaa_samples), color.color_base, uint32_t(color.color_format),
                  depth.depth_base, uint32_t(depth.depth_format), uint32_t(mode.edram_mode));
    s += buf;
    auto woff = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    auto stl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    auto sbr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    std::snprintf(buf, sizeof(buf), " | win %d,%d scissor %u,%u-%u,%u%s", int(woff.window_x_offset),
                  int(woff.window_y_offset), stl.tl_x, stl.tl_y, sbr.br_x, sbr.br_y,
                  stl.window_offset_disable ? " (offset off)" : "");
    s += buf;
    auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    std::snprintf(buf, sizeof(buf), " | vport %g,%g %g,%g %g,%g vte x%u%u y%u%u z%u%u xyfmt %u",
                  regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE), regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
                  regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE), regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
                  regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE), regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
                  vte.vport_x_scale_ena, vte.vport_x_offset_ena, vte.vport_y_scale_ena, vte.vport_y_offset_ena,
                  vte.vport_z_scale_ena, vte.vport_z_offset_ena, vte.vtx_xy_fmt);
    s += buf;
    auto su = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    auto blend = regs.Get<reg::RB_BLENDCONTROL>();
    auto cc = regs.Get<reg::RB_COLORCONTROL>();
    auto dc = regs.Get<reg::RB_DEPTHCONTROL>();
    auto mask = regs.Get<reg::RB_COLOR_MASK>();
    std::snprintf(buf, sizeof(buf),
                  " | cull f%u b%u face %u | blend %u/%u/%u a %u/%u/%u mask %u%u%u%u | alpha test %u func %u ref %g"
                  " | z %u write %u func %u stencil %u",
                  su.cull_front, su.cull_back, su.face, uint32_t(blend.color_srcblend), uint32_t(blend.color_comb_fcn),
                  uint32_t(blend.color_destblend), uint32_t(blend.alpha_srcblend), uint32_t(blend.alpha_comb_fcn),
                  uint32_t(blend.alpha_destblend), mask.write_red0, mask.write_green0, mask.write_blue0,
                  mask.write_alpha0, cc.alpha_test_enable, uint32_t(cc.alpha_func), regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF),
                  dc.z_enable, dc.z_write_enable, uint32_t(dc.zfunc), dc.stencil_enable);
    s += buf;
    if (vs) {
      for (const auto& vb : vs->vertex_bindings()) {
        auto fetch = regs.GetVertexFetch(vb.fetch_constant);
        std::snprintf(buf, sizeof(buf), " | vb vf%u addr %08X size %u words endian %u stride %u",
                      vb.fetch_constant, fetch.address << 2, fetch.size, uint32_t(fetch.endian), vb.stride_words);
        s += buf;
        if (fetch.size <= 48 && memory_) {
          // Small buffers (the clear and resolve rectangles): the words as floats, swapped by the fetch's endian.
          const uint32_t* words = reinterpret_cast<const uint32_t*>(memory_->TranslatePhysical(fetch.address << 2));
          s += " [";
          for (uint32_t i = 0; i < fetch.size; i++) {
            uint32_t w = words[i];
            if (fetch.endian == xenos::Endian::k8in32) w = __builtin_bswap32(w);
            float f;
            std::memcpy(&f, &w, 4);
            std::snprintf(buf, sizeof(buf), "%s%g", i ? " " : "", f);
            s += buf;
          }
          s += "]";
        }
      }
    }
    if (mode.edram_mode == xenos::EdramMode::kCopy) {
      // A resolve: the SDK's decoding of the copy registers and the rectangle (the draw's vertices).
      draw_util::ResolveInfo info;
      if (draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info)) {
        std::snprintf(buf, sizeof(buf),
                      " | RESOLVE src %u %s edram base %u pitch %u fmt %u msaa %u | rect x %u y %u w %u h %u | dest %08X pitch %u"
                      " height %u fmt %u endian %u | clear color %u (%08X) depth %u (%08X) | copy cmd %u",
                      info.rb_copy_control.copy_src_select, info.IsCopyingDepth() ? "depth" : "color",
                      info.IsCopyingDepth() ? info.depth_edram_info.base_tiles : info.color_edram_info.base_tiles,
                      info.IsCopyingDepth() ? info.depth_edram_info.pitch_tiles : info.color_edram_info.pitch_tiles,
                      info.IsCopyingDepth() ? info.depth_edram_info.format : info.color_edram_info.format,
                      uint32_t(info.IsCopyingDepth() ? info.depth_edram_info.msaa_samples : info.color_edram_info.msaa_samples),
                      info.coordinate_info.edram_offset_x_div_8 * 8, info.coordinate_info.edram_offset_y_div_8 * 8,
                      info.coordinate_info.width_div_8 * 8, info.height_div_8 * 8, info.copy_dest_base,
                      info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32,
                      info.copy_dest_coordinate_info.height_aligned_div_32 * 32,
                      uint32_t(info.copy_dest_info.copy_dest_format), uint32_t(info.copy_dest_info.copy_dest_endian),
                      info.IsClearingColor(), info.rb_color_clear, info.IsClearingDepth(), info.rb_depth_clear,
                      uint32_t(info.rb_copy_control.copy_command));
        s += buf;
      } else {
        s += " | RESOLVE (GetResolveInfo failed)";
      }
    }
    if (ps) {
      for (const auto& tb : ps->texture_bindings()) {
        auto fetch = regs.GetTextureFetch(tb.fetch_constant);
        TextureInfo info;
        if (TextureInfo::Prepare(fetch, &info)) {
          std::snprintf(buf, sizeof(buf), " | tex tf%u %s %ux%u base %08X pitch %u tiled %u endian %u dim %u swz %03X filt %u/%u",
                        tb.fetch_constant, info.format_info()->name, info.width, info.height,
                        fetch.base_address << 12, info.pitch, info.is_tiled, uint32_t(info.endianness),
                        uint32_t(info.dimension), fetch.swizzle, uint32_t(fetch.mag_filter), uint32_t(fetch.min_filter));
        } else {
          std::snprintf(buf, sizeof(buf), " | tex tf%u (not preparable) base %08X", tb.fetch_constant,
                        fetch.base_address << 12);
        }
        s += buf;
      }
    }
    LogOnce("draw", s);
    return true;
  }

  bool IssueCopy() override {
    copies_++;
    frame_copies_++;
    const RegisterFile& regs = *register_file_;
    auto control = regs.Get<reg::RB_COPY_CONTROL>();
    auto dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
    auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
    auto surf = regs.Get<reg::RB_SURFACE_INFO>();
    auto color = regs.Get<reg::RB_COLOR_INFO>();
    auto depth = regs.Get<reg::RB_DEPTH_INFO>();
    auto woff = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
    auto stl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    auto sbr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    char buf[400];
    std::snprintf(buf, sizeof(buf),
                  "src %u sample %u cmd %u clear color %u depth %u | rt pitch %u msaa %u color base %u fmt %u depth base %u"
                  " | dest base %08X pitch %u height %u fmt %u num %u endian %u swap %u | win %d,%d scissor %u,%u-%u,%u"
                  " | clear color %08X depth %08X",
                  control.copy_src_select, uint32_t(control.copy_sample_select), uint32_t(control.copy_command),
                  control.color_clear_enable, control.depth_clear_enable, surf.surface_pitch,
                  uint32_t(surf.msaa_samples), color.color_base, uint32_t(color.color_format), depth.depth_base,
                  regs[XE_GPU_REG_RB_COPY_DEST_BASE], dest_pitch.copy_dest_pitch, dest_pitch.copy_dest_height,
                  uint32_t(dest_info.copy_dest_format), uint32_t(dest_info.copy_dest_number),
                  uint32_t(dest_info.copy_dest_endian), dest_info.copy_dest_swap, int(woff.window_x_offset),
                  int(woff.window_y_offset), stl.tl_x, stl.tl_y, sbr.br_x, sbr.br_y,
                  regs[XE_GPU_REG_RB_COLOR_CLEAR], regs[XE_GPU_REG_RB_DEPTH_CLEAR]);
    LogOnce("resolve", buf);
    return true;
  }

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width, uint32_t frontbuffer_height) override {
    swaps_++;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "front %08X %ux%u", frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
    LogOnce("swap", buf);
    if (swaps_ <= 3 || swaps_ % 60 == 0) {
      REXLOG_INFO("[trace] swap #{}: {} draws, {} resolves this frame; {} draws, {} resolves total; {} distinct states",
                  swaps_, frame_draws_, frame_copies_, draws_, copies_, seen_.size());
      std::printf("[nx] gpu: swap #%u\n", swaps_);  // the Eden gate's frame counter
      std::fflush(stdout);
    }
    frame_draws_ = 0;
    frame_copies_ = 0;
  }

 private:
  static uint64_t ShaderHash(const uint32_t* dwords, uint32_t count) { return cvhd::UcodeHash(dwords, count); }
  void LogOnce(const char* what, const std::string& s) {
    uint64_t key = Fnv1a(s);
    if (!seen_.insert(key).second) return;
    REXLOG_INFO("[trace] {} #{} (swap {}): {}", what, seen_.size(), swaps_, s);
  }

  std::unordered_map<uint64_t, Shader*> shaders_;
  std::unordered_set<uint64_t> seen_;
  uint32_t draws_ = 0, copies_ = 0, swaps_ = 0, frame_draws_ = 0, frame_copies_ = 0;
};

class TraceGraphicsSystem : public GraphicsSystem {
 public:
  std::string name() const override { return "Trace"; }
  // No provider, no presenter: the window stays without one, as in the null backend.
  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext*) override { return X_STATUS_SUCCESS; }

 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<TraceCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace

}  // namespace rex::graphics::trace

#if defined(__SWITCH__) || defined(__linux__)
namespace rex::graphics::deko {
// deko3d_backend.cpp: always linked on the Switch; on Linux only with -DCVHD_DK_GL_HOST=ON (weak: null without it).
// The Switch reference must stay strong: a weak one never pulls deko3d_backend.o out of the static plugin
// archive, and 0.292 shipped without the backend ("requested backend 'deko3d' is not compiled").
#ifdef __SWITCH__
rex::system::IGraphicsSystem* CreateDeko3dGraphicsSystem();
#else
__attribute__((weak)) rex::system::IGraphicsSystem* CreateDeko3dGraphicsSystem();
#endif
}
#endif

extern "C" rex::system::IGraphicsSystem* rex_gpu_extra_backend(const char* name) {
  if (name && std::strcmp(name, "trace") == 0) {
    return new rex::graphics::trace::TraceGraphicsSystem();
  }
#if defined(__SWITCH__) || defined(__linux__)
  if (name && std::strcmp(name, "deko3d") == 0 && &rex::graphics::deko::CreateDeko3dGraphicsSystem) {
    return rex::graphics::deko::CreateDeko3dGraphicsSystem();
  }
#endif
  return nullptr;
}
