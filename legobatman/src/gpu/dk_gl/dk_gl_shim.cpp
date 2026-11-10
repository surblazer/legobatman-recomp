// OpenGL stand-in for the part of libdeko3d that src/gpu/deko3d_backend.cpp uses, so the real backend
// code runs on Linux (headless EGL, Mesa llvmpipe) next to the Vulkan reference backend:
// the same register decoding, texture handling, resolve aliasing and shaders (the translator's GLSL
// instead of DKSH), with the frames written as PPM files. A test tool, never part of the Switch build.
//
// Semantics follow libdeko3d (devkitPro/deko3d, ZLIB), checked against its source:
//   - NDC y = +1 maps to image row 0 (default flags: YAxisPointsUp + OriginUpperLeft): glClipControl
//     UPPER_LEFT; depth range 0..1: ZERO_TO_ONE.
//   - 16-bit packed formats keep deko3d's channel order (format_traits.inc): RGBA4/RGB565/RGB5A1 have
//     R in the low bits, BGR565/BGR5A1 have R in the high bits.
//   - Commands are recorded and run at dkQueueSubmitCommands, reading the memory blocks then (as the GPU
//     does), so a backend that rewrites memory before the GPU has used it shows the same fault here.
// Env: CVHD_DK_GL_DUMP=<dir>[:<every N swaps>] writes <dir>/swap-NNNNNN.ppm (default every 30).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>
#include <GL/glext.h>
#include <dlfcn.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "deko3d.h"

#include "dk_gl_shim.h"

namespace {

// ----- GL function loading ------------------------------------------------------------------------
#define GL_FUNCS(X) \
  X(Viewport, VIEWPORT) X(Scissor, SCISSOR) X(Enable, ENABLE) X(Disable, DISABLE) X(DepthRangef, DEPTHRANGEF) \
  X(ClipControl, CLIPCONTROL) X(ColorMask, COLORMASK) X(DepthMask, DEPTHMASK) X(DepthFunc, DEPTHFUNC) \
  X(BlendFuncSeparate, BLENDFUNCSEPARATE) X(BlendEquationSeparate, BLENDEQUATIONSEPARATE) \
  X(ClearBufferfv, CLEARBUFFERFV) X(ClearBufferfi, CLEARBUFFERFI) X(GenFramebuffers, GENFRAMEBUFFERS) \
  X(BindFramebuffer, BINDFRAMEBUFFER) X(FramebufferTexture2D, FRAMEBUFFERTEXTURE2D) X(DrawBuffers, DRAWBUFFERS) \
  X(ReadBuffer, READBUFFER) X(CheckFramebufferStatus, CHECKFRAMEBUFFERSTATUS) X(BlitFramebuffer, BLITFRAMEBUFFER) \
  X(GenTextures, GENTEXTURES) X(BindTexture, BINDTEXTURE) X(TexStorage2D, TEXSTORAGE2D) \
  X(TexSubImage2D, TEXSUBIMAGE2D) X(CompressedTexSubImage2D, COMPRESSEDTEXSUBIMAGE2D) X(TexParameteri, TEXPARAMETERI) \
  X(ActiveTexture, ACTIVETEXTURE) X(BindSampler, BINDSAMPLER) X(GenSamplers, GENSAMPLERS) \
  X(SamplerParameteri, SAMPLERPARAMETERI) X(SamplerParameterfv, SAMPLERPARAMETERFV) \
  X(CopyImageSubData, COPYIMAGESUBDATA) X(PixelStorei, PIXELSTOREI) X(ReadPixels, READPIXELS) \
  X(GenVertexArrays, GENVERTEXARRAYS) X(BindVertexArray, BINDVERTEXARRAY) X(GenBuffers, GENBUFFERS) \
  X(BindBuffer, BINDBUFFER) X(BufferData, BUFFERDATA) X(BindBufferRange, BINDBUFFERRANGE) \
  X(VertexAttribFormat, VERTEXATTRIBFORMAT) X(VertexAttribIFormat, VERTEXATTRIBIFORMAT) \
  X(VertexAttribBinding, VERTEXATTRIBBINDING) X(EnableVertexAttribArray, ENABLEVERTEXATTRIBARRAY) \
  X(DisableVertexAttribArray, DISABLEVERTEXATTRIBARRAY) X(BindVertexBuffer, BINDVERTEXBUFFER) \
  X(DrawArrays, DRAWARRAYS) X(DrawElements, DRAWELEMENTS) X(CreateShader, CREATESHADER) \
  X(ShaderSource, SHADERSOURCE) X(CompileShader, COMPILESHADER) X(GetShaderiv, GETSHADERIV) \
  X(GetShaderInfoLog, GETSHADERINFOLOG) X(CreateProgram, CREATEPROGRAM) X(AttachShader, ATTACHSHADER) \
  X(LinkProgram, LINKPROGRAM) X(GetProgramiv, GETPROGRAMIV) X(GetProgramInfoLog, GETPROGRAMINFOLOG) \
  X(UseProgram, USEPROGRAM) X(DeleteShader, DELETESHADER) X(CullFace, CULLFACE) X(FrontFace, FRONTFACE) \
  X(PolygonOffset, POLYGONOFFSET) X(StencilFuncSeparate, STENCILFUNCSEPARATE) X(StencilOpSeparate, STENCILOPSEPARATE) \
  X(StencilMaskSeparate, STENCILMASKSEPARATE) \
  X(PolygonMode, POLYGONMODE) X(GetError, GETERROR) X(Finish, FINISH) X(ClearColor, CLEARCOLOR) \
  X(Clear, CLEAR) X(BlendColor, BLENDCOLOR) X(GetTexImage, GETTEXIMAGE) X(DeleteProgram, DELETEPROGRAM)

#define DECL(name, upper) PFNGL##upper##PROC gl##name = nullptr;
GL_FUNCS(DECL)
#undef DECL
PFNGLGETSTRINGPROC glGetStringFn = nullptr;

// ----- objects --------------------------------------------------------------------------------------
struct ImageData {
  GLuint tex = 0;
  GLenum internal = 0;
  uint32_t w = 0, h = 0;
  DkImageFormat fmt = DkImageFormat_None;
  bool depth = false;
};
static_assert(sizeof(ImageData) <= sizeof(DkImage), "image storage");
struct LayoutData {
  DkImageFormat fmt;
  uint32_t w, h, flags;
};
static_assert(sizeof(LayoutData) <= sizeof(DkImageLayout), "layout storage");
struct ImageDescData {
  const ImageData* image;
  uint8_t swizzle[4];
};
static_assert(sizeof(ImageDescData) <= sizeof(DkImageDescriptor), "descriptor storage");
struct SamplerDescData {
  uint8_t min, mag, mip, wrap[3];
  uint32_t border[4];
};
static_assert(sizeof(SamplerDescData) <= sizeof(DkSamplerDescriptor), "sampler storage");
struct ShaderData {
  uint32_t id;  // index into g_shaders
  DkStage stage;
};
static_assert(sizeof(ShaderData) <= sizeof(DkShader), "shader storage");

struct FormatInfo {
  GLenum internal, format, type;
  uint32_t bytes_per_texel;  // 0 for block formats
  uint32_t block_bytes;      // block formats: bytes per 4x4 block
  bool compressed;
  bool depth;
};

bool FormatOf(DkImageFormat f, FormatInfo& o) {
  o = {};
  switch (f) {
    case DkImageFormat_R8_Unorm: o = {GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1}; return true;
    case DkImageFormat_RG8_Unorm: o = {GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2}; return true;
    case DkImageFormat_RGBA8_Unorm: o = {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return true;
    case DkImageFormat_RGBA8_Unorm_sRGB: o = {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4}; return true;
    case DkImageFormat_R16_Float: o = {GL_R16F, GL_RED, GL_HALF_FLOAT, 2}; return true;
    case DkImageFormat_R16_Unorm: o = {GL_R16, GL_RED, GL_UNSIGNED_SHORT, 2}; return true;
    case DkImageFormat_RG16_Unorm: o = {GL_RG16, GL_RG, GL_UNSIGNED_SHORT, 4}; return true;
    case DkImageFormat_R32_Float: o = {GL_R32F, GL_RED, GL_FLOAT, 4}; return true;
    case DkImageFormat_RGBA16_Float: o = {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8}; return true;
    case DkImageFormat_RGB10A2_Unorm: o = {GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4}; return true;
    // deko3d's packed 16-bit formats (format_traits.inc): RGBA4/RGB565/RGB5A1 are A4B4G4R4 / B5G6R5 /
    // A1B5G5R5 in memory with the components read straight (R = the low field); BGR565/BGR5A1 read R
    // from the high field (Xenos order).
    case DkImageFormat_RGBA4_Unorm: o = {GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2}; return true;
    case DkImageFormat_RGB565_Unorm: o = {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5_REV, 2}; return true;
    case DkImageFormat_RGB5A1_Unorm: o = {GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2}; return true;
    case DkImageFormat_BGR565_Unorm: o = {GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2}; return true;
    case DkImageFormat_BGR5A1_Unorm: o = {GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2}; return true;
    case DkImageFormat_RGBA_BC1: case DkImageFormat_RGB_BC1: o = {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 0, 8, true}; return true;
    case DkImageFormat_RGBA_BC2: o = {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 0, 16, true}; return true;
    case DkImageFormat_RGBA_BC3: o = {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 0, 16, true}; return true;
    case DkImageFormat_RG_BC5_Unorm: o = {GL_COMPRESSED_RG_RGTC2, 0, 0, 0, 16, true}; return true;
    case DkImageFormat_R_BC4_Unorm: o = {GL_COMPRESSED_RED_RGTC1, 0, 0, 0, 8, true}; return true;
    case DkImageFormat_Z24S8: o = {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 0, false, true}; return true;
    case DkImageFormat_ZF32: o = {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, 0, false, true}; return true;
    case DkImageFormat_Z16: o = {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, 0, false, true}; return true;
    default: return false;
  }
}

struct MemBlockData {
  std::vector<uint8_t> bytes;
  uint32_t index = 0;
};

struct Op;
struct CmdListData {
  std::vector<std::function<void()>> ops;
};

struct GlslShader {
  std::string source;
  DkStage stage;
};

struct Globals {
  EGLDisplay display = nullptr;
  EGLContext context = nullptr;
  bool ok = false;
  std::vector<MemBlockData*> blocks;
  std::vector<GlslShader> shaders;
  std::map<uint64_t, GLuint> programs;
  std::map<uint64_t, GLuint> samplers;  // SamplerDescData content -> GL sampler
  GLuint fbo = 0, fbo_read = 0, vao = 0;
  GLuint scratch_buffers[3] = {};
  uint32_t dump_every = 30;
  uint32_t dump_from = 0, dump_until = 0;  // LEGO Batman port: CVHD_DK_GL_DUMP_RANGE=<from>-<until> (every swap)
  std::string dump_dir;
  uint32_t presents = 0;
  uint64_t gl_errors = 0;
};
Globals G;

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::printf("[dkgl] ");
  std::vprintf(fmt, ap);
  std::printf("\n");
  va_end(ap);
  std::fflush(stdout);
}

void CheckGl(const char* where) {
  GLenum e;
  while ((e = glGetError()) != GL_NO_ERROR) {
    if (G.gl_errors++ < 40) Log("GL error 0x%x after %s", e, where);
  }
}

uint8_t* HostPtr(DkGpuAddr addr, uint32_t* block_size = nullptr) {
  uint32_t idx = uint32_t(addr >> 32) - 1;
  if (idx >= G.blocks.size()) return nullptr;
  MemBlockData* b = G.blocks[idx];
  uint32_t off = uint32_t(addr);
  if (off > b->bytes.size()) return nullptr;
  if (block_size) *block_size = uint32_t(b->bytes.size()) - off;
  return b->bytes.data() + off;
}

// ----- executor state (the recorded commands mutate it, draws apply it) ---------------------------------
struct VtxBufBinding {
  DkGpuAddr addr = 0;
  uint32_t size = 0;
};
struct Exec {
  const ImageData* color = nullptr;
  const ImageData* depth = nullptr;
  DkViewport viewport{0, 0, 1, 1, 0, 1};
  DkScissor scissor{0, 0, 1, 1};
  DkRasterizerState rast{};
  DkColorState colorstate{};
  DkColorWriteState colorwrite{};
  DkBlendState blend{};
  DkDepthStencilState ds{};
  float bias_constant = 0, bias_slope = 0;  // LEGO Batman port: depth bias and stencil
  uint8_t st_mask[2] = {0xFF, 0xFF}, st_ref[2] = {}, st_fmask[2] = {0xFF, 0xFF};
  const DkShader* shaders[5] = {};  // by stage: vertex, tess ctrl, tess eval, geometry, fragment
  std::vector<DkVtxAttribState> attribs;
  std::vector<DkVtxBufferState> vbufs;
  VtxBufBinding vtx[DK_MAX_VERTEX_BUFFERS] = {};
  DkIdxFormat idx_format = DkIdxFormat_Uint16;
  DkGpuAddr idx_addr = 0;
  DkResHandle tex[DK_NUM_TEXTURE_BINDINGS] = {};
  bool tex_bound[DK_NUM_TEXTURE_BINDINGS] = {};
  DkBufExtents ubo[2][DK_NUM_UNIFORM_BUFS] = {};  // [0] vertex, [1] fragment
  DkGpuAddr image_desc_set = 0, sampler_desc_set = 0;
  bool inited = false;
} E;

// libEGL is opened at run time (the plugin has no link dependency on it): every egl call goes through a
// pointer taken with dlsym.
void* g_egl = nullptr;
#define EGLF(ret, name, args) ret (*name##P) args = nullptr;
EGLF(void*, eglGetProcAddress, (const char*))
EGLF(EGLDisplay, eglGetDisplay, (EGLNativeDisplayType))
EGLF(EGLBoolean, eglInitialize, (EGLDisplay, EGLint*, EGLint*))
EGLF(EGLBoolean, eglBindAPI, (EGLenum))
EGLF(EGLBoolean, eglChooseConfig, (EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*))
EGLF(EGLContext, eglCreateContext, (EGLDisplay, EGLConfig, EGLContext, const EGLint*))
EGLF(EGLBoolean, eglMakeCurrent, (EGLDisplay, EGLSurface, EGLSurface, EGLContext))
EGLF(EGLint, eglGetError, ())
#undef EGLF
#define eglGetProcAddress eglGetProcAddressP
#define eglGetDisplay eglGetDisplayP
#define eglInitialize eglInitializeP
#define eglBindAPI eglBindAPIP
#define eglChooseConfig eglChooseConfigP
#define eglCreateContext eglCreateContextP
#define eglMakeCurrent eglMakeCurrentP
#define eglGetError eglGetErrorP

void InitGlOnce() {
  if (G.ok || G.display) return;
  g_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
  if (!g_egl) { Log("no libEGL.so.1"); return; }
#define SYM(name) name##P = reinterpret_cast<decltype(name##P)>(dlsym(g_egl, #name));
  SYM(eglGetProcAddress) SYM(eglGetDisplay) SYM(eglInitialize) SYM(eglBindAPI) SYM(eglChooseConfig) SYM(eglCreateContext)
  SYM(eglMakeCurrent) SYM(eglGetError)
#undef SYM
  auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
  G.display = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint maj, min;
  if (!eglInitialize(G.display, &maj, &min)) { Log("eglInitialize failed"); return; }
  eglBindAPI(EGL_OPENGL_API);
  EGLint cfga[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  eglChooseConfig(G.display, cfga, &cfg, 1, &n);
  EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5, EGL_CONTEXT_OPENGL_PROFILE_MASK,
                 EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  G.context = eglCreateContext(G.display, cfg, EGL_NO_CONTEXT, ca);
  if (!G.context) { Log("eglCreateContext failed (0x%x)", eglGetError()); return; }
  eglMakeCurrent(G.display, EGL_NO_SURFACE, EGL_NO_SURFACE, G.context);
#define LOAD(name, upper) gl##name = reinterpret_cast<PFNGL##upper##PROC>(eglGetProcAddress("gl" #name));
  GL_FUNCS(LOAD)
#undef LOAD
  glGetStringFn = reinterpret_cast<PFNGLGETSTRINGPROC>(eglGetProcAddress("glGetString"));
  Log("OpenGL %s on %s", glGetStringFn(GL_VERSION), glGetStringFn(GL_RENDERER));
  glGenFramebuffers(1, &G.fbo);
  glGenFramebuffers(1, &G.fbo_read);
  glGenVertexArrays(1, &G.vao);
  glBindVertexArray(G.vao);
  glGenBuffers(3, G.scratch_buffers);
  glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
  glEnable(GL_FRAMEBUFFER_SRGB);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  if (const char* d = std::getenv("CVHD_DK_GL_DUMP")) {
    G.dump_dir = d;
    size_t c = G.dump_dir.rfind(':');
    if (c != std::string::npos && c + 1 < G.dump_dir.size() && std::isdigit(static_cast<unsigned char>(G.dump_dir[c + 1]))) {
      G.dump_every = uint32_t(std::atoi(G.dump_dir.c_str() + c + 1));
      G.dump_dir.resize(c);
    }
    Log("frame dumps: %s every %u swaps", G.dump_dir.c_str(), G.dump_every);
    if (const char* r = std::getenv("CVHD_DK_GL_DUMP_RANGE")) {
      G.dump_from = uint32_t(std::atoi(r));
      if (const char* m = std::strchr(r, '-')) G.dump_until = uint32_t(std::atoi(m + 1));
    }
  }
  G.ok = true;
}

GLenum CompareFunc(DkCompareOp op) {
  switch (op) {
    case DkCompareOp_Never: return GL_NEVER;
    case DkCompareOp_Less: return GL_LESS;
    case DkCompareOp_Equal: return GL_EQUAL;
    case DkCompareOp_Lequal: return GL_LEQUAL;
    case DkCompareOp_Greater: return GL_GREATER;
    case DkCompareOp_NotEqual: return GL_NOTEQUAL;
    case DkCompareOp_Gequal: return GL_GEQUAL;
    default: return GL_ALWAYS;
  }
}

GLenum BlendFactorOf(DkBlendFactor f) {
  switch (f) {
    case DkBlendFactor_Zero: return GL_ZERO;
    case DkBlendFactor_One: return GL_ONE;
    case DkBlendFactor_SrcColor: return GL_SRC_COLOR;
    case DkBlendFactor_InvSrcColor: return GL_ONE_MINUS_SRC_COLOR;
    case DkBlendFactor_SrcAlpha: return GL_SRC_ALPHA;
    case DkBlendFactor_InvSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
    case DkBlendFactor_DstAlpha: return GL_DST_ALPHA;
    case DkBlendFactor_InvDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
    case DkBlendFactor_DstColor: return GL_DST_COLOR;
    case DkBlendFactor_InvDstColor: return GL_ONE_MINUS_DST_COLOR;
    case DkBlendFactor_SrcAlphaSaturate: return GL_SRC_ALPHA_SATURATE;
    case DkBlendFactor_ConstColor: return GL_CONSTANT_COLOR;
    case DkBlendFactor_InvConstColor: return GL_ONE_MINUS_CONSTANT_COLOR;
    case DkBlendFactor_ConstAlpha: return GL_CONSTANT_ALPHA;
    case DkBlendFactor_InvConstAlpha: return GL_ONE_MINUS_CONSTANT_ALPHA;
    default: return GL_ONE;
  }
}

GLenum BlendEq(DkBlendOp op) {
  switch (op) {
    case DkBlendOp_Sub: return GL_FUNC_SUBTRACT;
    case DkBlendOp_RevSub: return GL_FUNC_REVERSE_SUBTRACT;
    case DkBlendOp_Min: return GL_MIN;
    case DkBlendOp_Max: return GL_MAX;
    default: return GL_FUNC_ADD;
  }
}

GLenum WrapOf(uint8_t w) {
  switch (DkWrapMode(w)) {
    case DkWrapMode_Repeat: return GL_REPEAT;
    case DkWrapMode_MirroredRepeat: return GL_MIRRORED_REPEAT;
    case DkWrapMode_ClampToEdge: return GL_CLAMP_TO_EDGE;
    case DkWrapMode_ClampToBorder: return GL_CLAMP_TO_BORDER;
    case DkWrapMode_MirrorClampToEdge: case DkWrapMode_MirrorClamp: case DkWrapMode_MirrorClampToBorder: return GL_MIRROR_CLAMP_TO_EDGE;
    default: return GL_CLAMP_TO_EDGE;
  }
}

GLint SwizzleOf(uint8_t s) {
  switch (DkImageSwizzle(s)) {
    case DkImageSwizzle_Zero: return GL_ZERO;
    case DkImageSwizzle_One: return GL_ONE;
    case DkImageSwizzle_Red: return GL_RED;
    case DkImageSwizzle_Green: return GL_GREEN;
    case DkImageSwizzle_Blue: return GL_BLUE;
    default: return GL_ALPHA;
  }
}

GLuint SamplerFor(const SamplerDescData& s) {
  uint64_t key = 0;
  std::memcpy(&key, &s, std::min<size_t>(sizeof(key), 8));
  key = key * 1099511628211ull ^ (uint64_t(s.border[0]) << 1) ^ s.border[3];
  auto it = G.samplers.find(key);
  if (it != G.samplers.end()) return it->second;
  GLuint id;
  glGenSamplers(1, &id);
  glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, s.min == DkFilter_Linear ? GL_LINEAR : GL_NEAREST);
  glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, s.mag == DkFilter_Linear ? GL_LINEAR : GL_NEAREST);
  glSamplerParameteri(id, GL_TEXTURE_WRAP_S, WrapOf(s.wrap[0]));
  glSamplerParameteri(id, GL_TEXTURE_WRAP_T, WrapOf(s.wrap[1]));
  float border[4];
  std::memcpy(border, s.border, sizeof(border));
  glSamplerParameterfv(id, GL_TEXTURE_BORDER_COLOR, border);
  G.samplers.emplace(key, id);
  return id;
}

std::string PatchGlsl(std::string src, DkStage stage) {
  // GL 4.5 (llvmpipe) instead of 4.6; uniform block bindings are global in GL: the fragment stage's
  // go after the vertex stage's (binding n -> 8 + n).
  size_t v = src.find("#version 460");
  if (v != std::string::npos) src.replace(v, 12, "#version 450");
  if (stage == DkStage_Fragment) {
    // The present pass's fps counter block (binding 0 of the fragment stage, bound at 8 below).
    const std::string hud = "layout(std140, binding = 0) uniform Hud {";
    size_t h = src.find(hud);
    if (h != std::string::npos) src.replace(h, hud.size(), "layout(std140, binding = 8) uniform Hud {");
    // The ALU constants block of the two stages differs (GL has one interface block per name, with
    // global member names): the fragment stage's goes to binding 8 as ConstsF / cf. The system block
    // is the same data in both stages and keeps its name and binding 1.
    const std::string decl = "layout(std140, binding = 0) uniform Consts { vec4 c[256]; };";
    size_t at = src.find(decl);
    if (at != std::string::npos) {
      src.replace(at, decl.size(), "layout(std140, binding = 8) uniform ConstsF { vec4 cf[256]; };");
      size_t pos = at;
      while ((pos = src.find("c[", pos + 1)) != std::string::npos) {
        if (pos > 0 && (std::isalnum(static_cast<unsigned char>(src[pos - 1])) || src[pos - 1] == '_')) continue;
        src.replace(pos, 1, "cf");
      }
    }
  }
  return src;
}

GLuint ProgramFor(const ShaderData* vs, const ShaderData* gs, const ShaderData* fs) {
  uint64_t key = (uint64_t(vs->id + 1) << 40) | (uint64_t(gs ? gs->id + 1 : 0) << 20) | (fs->id + 1);
  auto it = G.programs.find(key);
  if (it != G.programs.end()) return it->second;
  GLuint prog = glCreateProgram();
  const ShaderData* list[3] = {vs, gs, fs};
  for (const ShaderData* s : list) {
    if (!s) continue;
    GLenum type = s->stage == DkStage_Vertex ? GL_VERTEX_SHADER : s->stage == DkStage_Geometry ? GL_GEOMETRY_SHADER : GL_FRAGMENT_SHADER;
    GLuint sh = glCreateShader(type);
    std::string text = PatchGlsl(G.shaders[s->id].source, s->stage);
    const char* p = text.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[2048];
      glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
      Log("shader %u (stage %d) failed to compile: %s", s->id, int(s->stage), log);
      glDeleteShader(sh);
      glDeleteProgram(prog);
      G.programs.emplace(key, 0);
      return 0;
    }
    glAttachShader(prog, sh);
    glDeleteShader(sh);
  }
  glLinkProgram(prog);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
    Log("program link failed: %s", log);
    glDeleteProgram(prog);
    prog = 0;
  }
  G.programs.emplace(key, prog);
  return prog;
}

void BindFramebuffer(const ImageData* color, const ImageData* depth) {
  glBindFramebuffer(GL_FRAMEBUFFER, G.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color ? color->tex : 0, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth ? depth->tex : 0, 0);
  {  // LEGO Batman port: report an incomplete combination (once per distinct status and sizes)
    static int reported = 0;
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE && reported++ < 10)
      Log("framebuffer incomplete 0x%x: colour %ux%u fmt 0x%x, depth %ux%u fmt 0x%x", st, color ? color->w : 0, color ? color->h : 0,
          color ? color->internal : 0, depth ? depth->w : 0, depth ? depth->h : 0, depth ? depth->internal : 0);
  }
  GLenum draw = color ? GL_COLOR_ATTACHMENT0 : GL_NONE;
  glDrawBuffers(1, &draw);
}

void ApplyScissorViewport() {
  const ImageData* t = E.color ? E.color : E.depth;
  glEnable(GL_SCISSOR_TEST);
  DkScissor s = E.scissor;
  if (t) {
    if (s.x >= t->w || s.y >= t->h) s.width = s.height = 0;
    else {
      s.width = std::min(s.width, t->w - s.x);
      s.height = std::min(s.height, t->h - s.y);
    }
  }
  glScissor(int(s.x), int(s.y), int(s.width), int(s.height));
  glViewport(int(E.viewport.x), int(E.viewport.y), int(E.viewport.width), int(E.viewport.height));
  glDepthRangef(E.viewport.near, E.viewport.far);
}

void ApplyFixedState() {
  uint32_t m = E.colorwrite.masks;
  glColorMask(m & DkColorMask_R, (m & DkColorMask_G) != 0, (m & DkColorMask_B) != 0, (m & DkColorMask_A) != 0);
  if (E.colorstate.blendEnableMask & 1) {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(BlendEq(E.blend.colorBlendOp), BlendEq(E.blend.alphaBlendOp));
    glBlendFuncSeparate(BlendFactorOf(E.blend.srcColorBlendFactor), BlendFactorOf(E.blend.dstColorBlendFactor),
                        BlendFactorOf(E.blend.srcAlphaBlendFactor), BlendFactorOf(E.blend.dstAlphaBlendFactor));
  } else {
    glDisable(GL_BLEND);
  }
  if (E.ds.depthTestEnable && E.depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
  glDepthMask(E.ds.depthWriteEnable && E.depth ? GL_TRUE : GL_FALSE);
  glDepthFunc(CompareFunc(DkCompareOp(E.ds.depthCompareOp)));
  // LEGO Batman port: stencil and depth bias (the backend sets them since the flicker hunt).
  if (E.ds.stencilTestEnable && E.depth) {
    auto sop = [](uint32_t o) -> GLenum {
      static const GLenum t[] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
      return o < 9 ? t[o] : GL_KEEP;
    };
    glEnable(GL_STENCIL_TEST);
    glStencilFuncSeparate(GL_FRONT, CompareFunc(DkCompareOp(E.ds.stencilFrontCompareOp)), E.st_ref[0], E.st_fmask[0]);
    glStencilFuncSeparate(GL_BACK, CompareFunc(DkCompareOp(E.ds.stencilBackCompareOp)), E.st_ref[1], E.st_fmask[1]);
    glStencilOpSeparate(GL_FRONT, sop(E.ds.stencilFrontFailOp), sop(E.ds.stencilFrontDepthFailOp), sop(E.ds.stencilFrontPassOp));
    glStencilOpSeparate(GL_BACK, sop(E.ds.stencilBackFailOp), sop(E.ds.stencilBackDepthFailOp), sop(E.ds.stencilBackPassOp));
    glStencilMaskSeparate(GL_FRONT, E.st_mask[0]);
    glStencilMaskSeparate(GL_BACK, E.st_mask[1]);
  } else {
    glDisable(GL_STENCIL_TEST);
  }
  if (E.rast.depthBiasEnableMask) {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(E.bias_slope, E.bias_constant);
  } else {
    glDisable(GL_POLYGON_OFFSET_FILL);
  }
  if (E.rast.cullMode == DkFace_None) glDisable(GL_CULL_FACE);
  else {
    glEnable(GL_CULL_FACE);
    glCullFace(E.rast.cullMode == DkFace_Front ? GL_FRONT : E.rast.cullMode == DkFace_Back ? GL_BACK : GL_FRONT_AND_BACK);
  }
  glFrontFace(E.rast.frontFace == DkFrontFace_CCW ? GL_CCW : GL_CW);
}

bool VertexFormatOf(const DkVtxAttribState& a, GLint& comps, GLenum& type, GLboolean& norm, bool& integer) {
  integer = false;
  norm = (a.type == DkVtxAttribType_Snorm || a.type == DkVtxAttribType_Unorm) ? GL_TRUE : GL_FALSE;
  bool is_signed = a.type == DkVtxAttribType_Snorm || a.type == DkVtxAttribType_Sscaled || a.type == DkVtxAttribType_Sint;
  bool is_float = a.type == DkVtxAttribType_Float;
  integer = a.type == DkVtxAttribType_Sint || a.type == DkVtxAttribType_Uint;
  switch (a.size) {
    case DkVtxAttribSize_1x32: comps = 1; type = is_float ? GL_FLOAT : is_signed ? GL_INT : GL_UNSIGNED_INT; return true;
    case DkVtxAttribSize_2x32: comps = 2; type = is_float ? GL_FLOAT : is_signed ? GL_INT : GL_UNSIGNED_INT; return true;
    case DkVtxAttribSize_3x32: comps = 3; type = is_float ? GL_FLOAT : is_signed ? GL_INT : GL_UNSIGNED_INT; return true;
    case DkVtxAttribSize_4x32: comps = 4; type = is_float ? GL_FLOAT : is_signed ? GL_INT : GL_UNSIGNED_INT; return true;
    case DkVtxAttribSize_1x16: comps = 1; type = is_float ? GL_HALF_FLOAT : is_signed ? GL_SHORT : GL_UNSIGNED_SHORT; return true;
    case DkVtxAttribSize_2x16: comps = 2; type = is_float ? GL_HALF_FLOAT : is_signed ? GL_SHORT : GL_UNSIGNED_SHORT; return true;
    case DkVtxAttribSize_3x16: comps = 3; type = is_float ? GL_HALF_FLOAT : is_signed ? GL_SHORT : GL_UNSIGNED_SHORT; return true;
    case DkVtxAttribSize_4x16: comps = 4; type = is_float ? GL_HALF_FLOAT : is_signed ? GL_SHORT : GL_UNSIGNED_SHORT; return true;
    case DkVtxAttribSize_1x8: comps = 1; type = is_signed ? GL_BYTE : GL_UNSIGNED_BYTE; return true;
    case DkVtxAttribSize_2x8: comps = 2; type = is_signed ? GL_BYTE : GL_UNSIGNED_BYTE; return true;
    case DkVtxAttribSize_3x8: comps = 3; type = is_signed ? GL_BYTE : GL_UNSIGNED_BYTE; return true;
    case DkVtxAttribSize_4x8: comps = a.isBgra ? GL_BGRA : 4; type = is_signed ? GL_BYTE : GL_UNSIGNED_BYTE; return true;
    case DkVtxAttribSize_10_10_10_2: comps = 4; type = is_signed ? GL_INT_2_10_10_10_REV : GL_UNSIGNED_INT_2_10_10_10_REV; return true;
    case DkVtxAttribSize_11_11_10: comps = 3; type = GL_UNSIGNED_INT_10F_11F_11F_REV; norm = GL_FALSE; return true;
    default: return false;
  }
}

GLuint ScratchBuffer(int slot, const void* data, size_t bytes) {
  glBindBuffer(GL_ARRAY_BUFFER, G.scratch_buffers[slot]);
  glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes ? bytes : 4), data, GL_STREAM_DRAW);
  return G.scratch_buffers[slot];
}

void BindDrawResources() {
  const ShaderData* vs = reinterpret_cast<const ShaderData*>(E.shaders[DkStage_Vertex]);
  const ShaderData* gs = reinterpret_cast<const ShaderData*>(E.shaders[DkStage_Geometry]);
  const ShaderData* fs = reinterpret_cast<const ShaderData*>(E.shaders[DkStage_Fragment]);
  (void)vs; (void)gs; (void)fs;
}

GLenum PrimitiveOf(DkPrimitive p, bool& quads) {
  quads = false;
  switch (p) {
    case DkPrimitive_Points: return GL_POINTS;
    case DkPrimitive_Lines: return GL_LINES;
    case DkPrimitive_LineLoop: return GL_LINE_LOOP;
    case DkPrimitive_LineStrip: return GL_LINE_STRIP;
    case DkPrimitive_Triangles: return GL_TRIANGLES;
    case DkPrimitive_TriangleStrip: return GL_TRIANGLE_STRIP;
    case DkPrimitive_TriangleFan: case DkPrimitive_Polygon: return GL_TRIANGLE_FAN;
    case DkPrimitive_Quads: quads = true; return GL_TRIANGLES;
    case DkPrimitive_QuadStrip: quads = true; return GL_TRIANGLES;
    default: return GL_TRIANGLES;
  }
}

// Runs the draw with the state accumulated in E. `indices` (host copy) is set for indexed draws.
void RunDraw(DkPrimitive prim, uint32_t count, uint32_t first, int32_t vertex_offset, bool indexed) {
  const ShaderData* vs = reinterpret_cast<const ShaderData*>(E.shaders[DkStage_Vertex]);
  const ShaderData* gs = reinterpret_cast<const ShaderData*>(E.shaders[DkStage_Geometry]);
  const ShaderData* fs = reinterpret_cast<const ShaderData*>(E.shaders[DkStage_Fragment]);
  if (!vs || !fs || (!E.color && !E.depth)) return;
  GLuint prog = ProgramFor(vs, gs, fs);
  if (!prog) return;
  BindFramebuffer(E.color, E.depth);
  ApplyScissorViewport();
  ApplyFixedState();
  glUseProgram(prog);
  // Uniform buffers: vertex stage 0.., fragment stage 8..
  for (int st = 0; st < 2; st++) {
    for (int i = 0; i < 4; i++) {
      const DkBufExtents& b = E.ubo[st][i];
      if (!b.size) continue;
      const uint8_t* p = HostPtr(b.addr);
      if (!p) continue;
      GLuint buf = G.scratch_buffers[2];
      (void)buf;
      GLuint ubo;
      static GLuint ubos[16] = {};
      if (!ubos[st * 8 + i]) glGenBuffers(1, &ubos[st * 8 + i]);
      ubo = ubos[st * 8 + i];
      glBindBuffer(GL_UNIFORM_BUFFER, ubo);
      glBufferData(GL_UNIFORM_BUFFER, b.size, p, GL_STREAM_DRAW);
      glBindBufferRange(GL_UNIFORM_BUFFER, st == 1 && i == 0 ? 8 : i, ubo, 0, b.size);
    }
  }
  // Textures (fragment stage only here).
  for (uint32_t i = 0; i < DK_NUM_TEXTURE_BINDINGS; i++) {
    if (!E.tex_bound[i]) continue;
    uint32_t image_id = E.tex[i] & ((1u << 20) - 1), sampler_id = E.tex[i] >> 20;
    const uint8_t* idesc = HostPtr(E.image_desc_set + uint64_t(image_id) * sizeof(DkImageDescriptor));
    const uint8_t* sdesc = HostPtr(E.sampler_desc_set + uint64_t(sampler_id) * sizeof(DkSamplerDescriptor));
    if (!idesc || !sdesc) continue;
    const ImageDescData* d = reinterpret_cast<const ImageDescData*>(idesc);
    const SamplerDescData* s = reinterpret_cast<const SamplerDescData*>(sdesc);
    if (!d->image) continue;
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, d->image->tex);
    GLint sw[4] = {SwizzleOf(d->swizzle[0]), SwizzleOf(d->swizzle[1]), SwizzleOf(d->swizzle[2]), SwizzleOf(d->swizzle[3])};
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, sw[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, sw[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, sw[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, sw[3]);
    glBindSampler(i, SamplerFor(*s));
  }
  // Vertex attributes.
  for (uint32_t i = 0; i < 16; i++) glDisableVertexAttribArray(i);
  for (uint32_t i = 0; i < E.attribs.size() && i < 16; i++) {
    const DkVtxAttribState& a = E.attribs[i];
    GLint comps;
    GLenum type;
    GLboolean norm;
    bool integer;
    if (!VertexFormatOf(a, comps, type, norm, integer)) continue;
    if (integer) glVertexAttribIFormat(i, comps, type, a.offset);
    else glVertexAttribFormat(i, comps, type, norm, a.offset);
    glVertexAttribBinding(i, a.bufferId);
    glEnableVertexAttribArray(i);
  }
  uint32_t vertex_extent = indexed ? 0 : first + count;  // vertices a non-indexed draw reads
  std::vector<uint8_t> index_scratch;
  uint32_t max_index = 0;
  const void* idx_ptr = nullptr;
  uint32_t idx_size = E.idx_format == DkIdxFormat_Uint32 ? 4 : E.idx_format == DkIdxFormat_Uint16 ? 2 : 1;
  if (indexed) {
    const uint8_t* ip = HostPtr(E.idx_addr);
    if (!ip) return;
    ip += size_t(first) * idx_size;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t v = idx_size == 4 ? reinterpret_cast<const uint32_t*>(ip)[i] : idx_size == 2 ? reinterpret_cast<const uint16_t*>(ip)[i] : ip[i];
      max_index = std::max(max_index, v);
    }
    idx_ptr = ip;
    vertex_extent = max_index + 1 + (vertex_offset > 0 ? vertex_offset : 0);
  }
  // Vertex buffers: upload what the draw can read.
  for (uint32_t b = 0; b < E.vbufs.size() && b < DK_MAX_VERTEX_BUFFERS; b++) {
    if (!E.vtx[b].addr) continue;
    const uint8_t* p = HostPtr(E.vtx[b].addr);
    if (!p) continue;
    uint32_t bytes = E.vtx[b].size;
    GLuint buf = ScratchBuffer(0, p, bytes);
    (void)buf;
    static GLuint vbos[DK_MAX_VERTEX_BUFFERS] = {};
    if (!vbos[b]) glGenBuffers(1, &vbos[b]);
    glBindBuffer(GL_ARRAY_BUFFER, vbos[b]);
    glBufferData(GL_ARRAY_BUFFER, bytes ? bytes : 4, p, GL_STREAM_DRAW);
    glBindVertexBuffer(b, vbos[b], 0, E.vbufs[b].stride);
  }
  (void)vertex_extent;
  bool quads;
  GLenum mode = PrimitiveOf(prim, quads);
  if (quads) {
    // Quad list -> triangles (v0 v1 v2, v0 v2 v3 per quad), through an index buffer.
    std::vector<uint32_t> tri;
    uint32_t quad_count = count / 4;
    auto fetch = [&](uint32_t k) -> uint32_t {
      if (!indexed) return first + k;
      return idx_size == 4 ? reinterpret_cast<const uint32_t*>(idx_ptr)[k]
                           : idx_size == 2 ? reinterpret_cast<const uint16_t*>(idx_ptr)[k] : reinterpret_cast<const uint8_t*>(idx_ptr)[k];
    };
    if (prim == DkPrimitive_QuadStrip) {
      for (uint32_t q = 0; q + 3 < count; q += 2) {
        uint32_t v0 = fetch(q), v1 = fetch(q + 1), v2 = fetch(q + 2), v3 = fetch(q + 3);
        tri.insert(tri.end(), {v0, v1, v3, v0, v3, v2});
      }
    } else {
      for (uint32_t q = 0; q < quad_count; q++) {
        uint32_t v0 = fetch(q * 4), v1 = fetch(q * 4 + 1), v2 = fetch(q * 4 + 2), v3 = fetch(q * 4 + 3);
        tri.insert(tri.end(), {v0, v1, v2, v0, v2, v3});
      }
    }
    static GLuint ibo = 0;
    if (!ibo) glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, tri.size() * 4, tri.data(), GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, GLsizei(tri.size()), GL_UNSIGNED_INT, nullptr);
  } else if (indexed) {
    static GLuint ibo = 0;
    if (!ibo) glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, size_t(count) * idx_size, idx_ptr, GL_STREAM_DRAW);
    glDrawElements(mode, GLsizei(count), idx_size == 4 ? GL_UNSIGNED_INT : idx_size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, nullptr);
  } else {
    glDrawArrays(mode, GLint(first), GLsizei(count));
  }
  CheckGl("draw");
}

void WritePpm(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h, bool flip) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%u %u\n255\n", w, h);
  std::vector<uint8_t> row(w * 3);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* src = rgba + size_t(flip ? h - 1 - y : y) * w * 4;
    for (uint32_t x = 0; x < w; x++) {
      row[x * 3] = src[x * 4];
      row[x * 3 + 1] = src[x * 4 + 1];
      row[x * 3 + 2] = src[x * 4 + 2];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

}  // namespace

// ----- public: shim extras --------------------------------------------------------------------------------
void dkglDumpImage(const DkImage* image, const char* path) {
  const ImageData* d = reinterpret_cast<const ImageData*>(image->_storage);
  if (!d || !d->tex || d->depth) return;
  std::vector<uint8_t> px(size_t(d->w) * d->h * 4);
  glBindTexture(GL_TEXTURE_2D, d->tex);
  glDisable(GL_FRAMEBUFFER_SRGB);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glEnable(GL_FRAMEBUFFER_SRGB);
  WritePpm(path, px.data(), d->w, d->h, false);
}

void dkShaderInitializeGlsl(DkShader* obj, DkStage stage, const char* text) {
  ShaderData* s = reinterpret_cast<ShaderData*>(obj->_storage);
  s->id = uint32_t(G.shaders.size());
  s->stage = stage;
  G.shaders.push_back({text, stage});
}

// ----- the deko3d API --------------------------------------------------------------------------------------
namespace dk::detail {
struct Device { int unused; };
struct MemBlock : MemBlockData {};
struct CmdBuf {
  std::vector<std::function<void()>> ops;
};
struct Queue { int unused; };
struct Swapchain {
  std::vector<const DkImage*> images;
  uint32_t next = 0;
};
}  // namespace dk::detail

extern "C" {

DkDevice dkDeviceCreate(DkDeviceMaker const*) {
  InitGlOnce();
  if (!G.ok) return nullptr;
  return new dk::detail::Device();
}
void dkDeviceDestroy(DkDevice d) { delete d; }

DkMemBlock dkMemBlockCreate(DkMemBlockMaker const* m) {
  auto* b = new dk::detail::MemBlock();
  b->bytes.assign(m->size, 0);
  b->index = uint32_t(G.blocks.size());
  G.blocks.push_back(b);
  return b;
}
void dkMemBlockDestroy(DkMemBlock b) { if (b) G.blocks[b->index] = nullptr; delete b; }
void* dkMemBlockGetCpuAddr(DkMemBlock b) { return b->bytes.data(); }
DkGpuAddr dkMemBlockGetGpuAddr(DkMemBlock b) { return uint64_t(b->index + 1) << 32; }
uint32_t dkMemBlockGetSize(DkMemBlock b) { return uint32_t(b->bytes.size()); }
DkResult dkMemBlockFlushCpuCache(DkMemBlock, uint32_t, uint32_t) { return DkResult_Success; }

DkResult dkFenceWait(DkFence*, int64_t) { return DkResult_Success; }

DkCmdBuf dkCmdBufCreate(DkCmdBufMaker const*) { return new dk::detail::CmdBuf(); }
void dkCmdBufDestroy(DkCmdBuf c) { delete c; }
void dkCmdBufAddMemory(DkCmdBuf, DkMemBlock, uint32_t, uint32_t) {}
void dkCmdBufClear(DkCmdBuf c) { c->ops.clear(); }
DkCmdList dkCmdBufFinishList(DkCmdBuf c) {
  auto* list = new CmdListData();
  list->ops = std::move(c->ops);
  c->ops.clear();
  return reinterpret_cast<DkCmdList>(list);
}
void dkCmdBufBarrier(DkCmdBuf, DkBarrier, uint32_t) {}
// LEGO Batman port: GPU timestamps (the backend's gpu ms/frame stat) are not measured on the GL host.
void dkCmdBufReportCounter(DkCmdBuf, DkCounter, DkGpuAddr) {}

void dkCmdBufBindShaders(DkCmdBuf c, uint32_t stageMask, DkShader const* const shaders[], uint32_t n) {
  std::vector<const DkShader*> list(shaders, shaders + n);
  c->ops.push_back([=] {
    // The listed shaders are given in stage order of the mask (vertex, ..., geometry, fragment): the
    // backend passes {vs, fs} or {vs, fs, gs}; map them by each shader's own stage.
    for (uint32_t st = 0; st < 5; st++) if (stageMask & (1u << st)) E.shaders[st] = nullptr;
    for (const DkShader* s : list) {
      const ShaderData* d = reinterpret_cast<const ShaderData*>(s->_storage);
      E.shaders[d->stage] = s;
    }
  });
}

void dkCmdBufBindUniformBuffers(DkCmdBuf c, DkStage stage, uint32_t firstId, DkBufExtents const buffers[], uint32_t n) {
  std::vector<DkBufExtents> v(buffers, buffers + n);
  c->ops.push_back([=] {
    int st = stage == DkStage_Vertex ? 0 : 1;
    for (uint32_t i = 0; i < v.size(); i++) E.ubo[st][firstId + i] = v[i];
  });
}

void dkCmdBufBindTextures(DkCmdBuf c, DkStage stage, uint32_t firstId, DkResHandle const handles[], uint32_t n) {
  std::vector<DkResHandle> v(handles, handles + n);
  c->ops.push_back([=] {
    if (stage != DkStage_Fragment) return;
    for (uint32_t i = 0; i < v.size(); i++) {
      E.tex[firstId + i] = v[i];
      E.tex_bound[firstId + i] = true;
    }
  });
}

void dkCmdBufBindImageDescriptorSet(DkCmdBuf c, DkGpuAddr addr, uint32_t) {
  c->ops.push_back([=] { E.image_desc_set = addr; });
}
void dkCmdBufBindSamplerDescriptorSet(DkCmdBuf c, DkGpuAddr addr, uint32_t) {
  c->ops.push_back([=] { E.sampler_desc_set = addr; });
}

void dkCmdBufBindRenderTargets(DkCmdBuf c, DkImageView const* const colorTargets[], uint32_t numColor, DkImageView const* depthTarget) {
  const ImageData* color = numColor ? reinterpret_cast<const ImageData*>(colorTargets[0]->pImage->_storage) : nullptr;
  const ImageData* depth = depthTarget ? reinterpret_cast<const ImageData*>(depthTarget->pImage->_storage) : nullptr;
  c->ops.push_back([=] { E.color = color; E.depth = depth; });
}

void dkCmdBufBindRasterizerState(DkCmdBuf c, DkRasterizerState const* s) { DkRasterizerState v = *s; c->ops.push_back([=] { E.rast = v; }); }
void dkCmdBufBindColorState(DkCmdBuf c, DkColorState const* s) { DkColorState v = *s; c->ops.push_back([=] { E.colorstate = v; }); }
void dkCmdBufBindColorWriteState(DkCmdBuf c, DkColorWriteState const* s) { DkColorWriteState v = *s; c->ops.push_back([=] { E.colorwrite = v; }); }
void dkCmdBufBindBlendStates(DkCmdBuf c, uint32_t, DkBlendState const states[], uint32_t) { DkBlendState v = states[0]; c->ops.push_back([=] { E.blend = v; }); }
void dkCmdBufBindDepthStencilState(DkCmdBuf c, DkDepthStencilState const* s) { DkDepthStencilState v = *s; c->ops.push_back([=] { E.ds = v; }); }
void dkCmdBufSetDepthBias(DkCmdBuf c, float constantFactor, float, float slopeFactor) {
  c->ops.push_back([=] { E.bias_constant = constantFactor; E.bias_slope = slopeFactor; });
}
void dkCmdBufSetStencil(DkCmdBuf c, DkFace face, uint8_t mask, uint8_t funcRef, uint8_t funcMask) {
  c->ops.push_back([=] {
    for (int i = 0; i < 2; i++)
      if (face == DkFace_FrontAndBack || (i == 0 && face == DkFace_Front) || (i == 1 && face == DkFace_Back)) {
        E.st_mask[i] = mask; E.st_ref[i] = funcRef; E.st_fmask[i] = funcMask;
      }
  });
}

void dkCmdBufBindVtxAttribState(DkCmdBuf c, DkVtxAttribState const attribs[], uint32_t n) {
  std::vector<DkVtxAttribState> v(attribs, attribs + n);
  c->ops.push_back([=] { E.attribs = v; });
}
void dkCmdBufBindVtxBufferState(DkCmdBuf c, DkVtxBufferState const buffers[], uint32_t n) {
  std::vector<DkVtxBufferState> v(buffers, buffers + n);
  c->ops.push_back([=] { E.vbufs = v; });
}
void dkCmdBufBindVtxBuffers(DkCmdBuf c, uint32_t first, DkBufExtents const buffers[], uint32_t n) {
  std::vector<DkBufExtents> v(buffers, buffers + n);
  c->ops.push_back([=] {
    for (uint32_t i = 0; i < v.size(); i++) E.vtx[first + i] = {v[i].addr, v[i].size};
  });
}
void dkCmdBufBindIdxBuffer(DkCmdBuf c, DkIdxFormat format, DkGpuAddr addr) {
  c->ops.push_back([=] { E.idx_format = format; E.idx_addr = addr; });
}

void dkCmdBufSetViewports(DkCmdBuf c, uint32_t, DkViewport const v[], uint32_t) { DkViewport vp = v[0]; c->ops.push_back([=] { E.viewport = vp; }); }
void dkCmdBufSetScissors(DkCmdBuf c, uint32_t, DkScissor const s[], uint32_t) { DkScissor sc = s[0]; c->ops.push_back([=] { E.scissor = sc; }); }

void dkCmdBufClearColor(DkCmdBuf c, uint32_t, uint32_t mask, const void* data) {
  float col[4];
  std::memcpy(col, data, sizeof(col));
  c->ops.push_back([=] {
    if (!E.color) return;
    BindFramebuffer(E.color, E.depth);
    ApplyScissorViewport();
    glColorMask((mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0, (mask & 8) != 0);
    glClearBufferfv(GL_COLOR, 0, col);
    CheckGl("clear color");
  });
}
void dkCmdBufClearDepthStencil(DkCmdBuf c, bool clearDepth, float depth, uint8_t stencilMask, uint8_t stencilValue) {
  c->ops.push_back([=] {
    if (!E.depth) return;
    BindFramebuffer(E.color, E.depth);
    ApplyScissorViewport();
    glDepthMask(GL_TRUE);
    if (clearDepth) glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, stencilMask ? stencilValue : 0);
    CheckGl("clear depth");
  });
}

void dkCmdBufDraw(DkCmdBuf c, DkPrimitive prim, uint32_t n, uint32_t, uint32_t first, uint32_t) {
  c->ops.push_back([=] { RunDraw(prim, n, first, 0, false); });
}
void dkCmdBufDrawIndexed(DkCmdBuf c, DkPrimitive prim, uint32_t n, uint32_t, uint32_t first, int32_t voff, uint32_t) {
  c->ops.push_back([=] { RunDraw(prim, n, first, voff, true); });
}

void dkCmdBufCopyImage(DkCmdBuf c, DkImageView const* src, DkImageRect const* sr, DkImageView const* dst, DkImageRect const* dr, uint32_t) {
  const ImageData* s = reinterpret_cast<const ImageData*>(src->pImage->_storage);
  const ImageData* d = reinterpret_cast<const ImageData*>(dst->pImage->_storage);
  DkImageRect a = *sr, b = *dr;
  c->ops.push_back([=] {
    if (s->internal == d->internal && !s->depth) {
      glCopyImageSubData(s->tex, GL_TEXTURE_2D, 0, int(a.x), int(a.y), 0, d->tex, GL_TEXTURE_2D, 0, int(b.x), int(b.y), 0, int(a.width), int(a.height), 1);
    } else {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, G.fbo_read);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, G.fbo);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, d->tex, 0);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
      GLenum db = GL_COLOR_ATTACHMENT0;
      glDrawBuffers(1, &db);
      glReadBuffer(GL_COLOR_ATTACHMENT0);
      glDisable(GL_SCISSOR_TEST);
      glDisable(GL_FRAMEBUFFER_SRGB);
      glBlitFramebuffer(int(a.x), int(a.y), int(a.x + a.width), int(a.y + a.height), int(b.x), int(b.y), int(b.x + a.width), int(b.y + a.height),
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glEnable(GL_FRAMEBUFFER_SRGB);
    }
    CheckGl("copy image");
  });
}

void dkCmdBufCopyBufferToImage(DkCmdBuf c, DkCopyBuf const* src, DkImageView const* dst, DkImageRect const* rect, uint32_t) {
  const ImageData* d = reinterpret_cast<const ImageData*>(dst->pImage->_storage);
  DkCopyBuf cb = *src;
  DkImageRect r = *rect;
  c->ops.push_back([=] {
    FormatInfo fi;
    if (!FormatOf(d->fmt, fi)) return;
    const uint8_t* p = HostPtr(cb.addr);
    if (!p) return;
    glBindTexture(GL_TEXTURE_2D, d->tex);
    if (fi.compressed) {
      uint32_t bw = (r.width + 3) / 4, bh = (r.height + 3) / 4;
      glCompressedTexSubImage2D(GL_TEXTURE_2D, 0, int(r.x), int(r.y), int(r.width), int(r.height), fi.internal, GLsizei(bw * bh * fi.block_bytes), p);
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, int(r.x), int(r.y), int(r.width), int(r.height), fi.format, fi.type, p);
    }
    CheckGl("upload");
  });
}

void dkCmdBufCopyImageToBuffer(DkCmdBuf c, DkImageView const* src, DkImageRect const* rect, DkCopyBuf const* dst, uint32_t) {
  const ImageData* s = reinterpret_cast<const ImageData*>(src->pImage->_storage);
  DkCopyBuf cb = *dst;
  DkImageRect r = *rect;
  c->ops.push_back([=] {
    uint8_t* p = HostPtr(cb.addr);
    if (!p) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, G.fbo_read);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(int(r.x), int(r.y), int(r.width), int(r.height), GL_RGBA, GL_UNSIGNED_BYTE, p);
  });
}

DkQueue dkQueueCreate(DkQueueMaker const*) { return new dk::detail::Queue(); }
void dkQueueDestroy(DkQueue q) { delete q; }
void dkQueueSignalFence(DkQueue, DkFence*, bool) {}
void dkQueueFlush(DkQueue) {}
void dkQueueWaitIdle(DkQueue) { glFinish(); }
void dkQueueSubmitCommands(DkQueue, DkCmdList list) {
  auto* l = reinterpret_cast<CmdListData*>(list);
  // Each submit starts from a clean slate of the cached bindings that a new list always sets itself.
  for (auto& op : l->ops) op();
  delete l;
}

DkSwapchain dkSwapchainCreate(DkSwapchainMaker const* m) {
  auto* s = new dk::detail::Swapchain();
  for (uint32_t i = 0; i < m->numImages; i++) s->images.push_back(m->pImages[i]);
  return s;
}
void dkSwapchainDestroy(DkSwapchain s) { delete s; }
int dkQueueAcquireImage(DkQueue, DkSwapchain s) {
  int slot = int(s->next);
  s->next = (s->next + 1) % uint32_t(s->images.size());
  return slot;
}
void dkQueuePresentImage(DkQueue, DkSwapchain s, int slot) {
  G.presents++;
  if (!G.dump_dir.empty() && ((G.dump_every && (G.presents % G.dump_every == 0 || G.presents == 1)) ||
                              (G.presents >= G.dump_from && G.presents < G.dump_until))) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/swap-%06u.ppm", G.dump_dir.c_str(), G.presents);
    dkglDumpImage(s->images[slot], path);
  }
}

void dkShaderInitialize(DkShader*, DkShaderMaker const*) {}  // DKSH is not used here: dkShaderInitializeGlsl

uint32_t dkImageFormatGetFlags(DkImageFormat f) {
  FormatInfo fi;
  return FormatOf(f, fi) && fi.depth ? DkImageFormatFlags_IsDepth : 0;
}

void dkImageLayoutInitialize(DkImageLayout* obj, DkImageLayoutMaker const* m) {
  auto* l = reinterpret_cast<LayoutData*>(obj->_storage);
  l->fmt = m->format;
  l->w = m->dimensions[0];
  l->h = m->dimensions[1];
  l->flags = m->flags;
}
uint64_t dkImageLayoutGetSize(DkImageLayout const* obj) {
  const auto* l = reinterpret_cast<const LayoutData*>(obj->_storage);
  FormatInfo fi;
  uint64_t bytes;
  if (!FormatOf(l->fmt, fi)) return 0;
  if (fi.compressed) bytes = uint64_t((l->w + 3) / 4) * ((l->h + 3) / 4) * fi.block_bytes;
  else bytes = uint64_t(l->w) * l->h * fi.bytes_per_texel;
  return (bytes + 0xFFF) & ~uint64_t(0xFFF);
}
uint32_t dkImageLayoutGetAlignment(DkImageLayout const*) { return 0x1000; }

void dkImageInitialize(DkImage* obj, DkImageLayout const* layout, DkMemBlock, uint32_t) {
  const auto* l = reinterpret_cast<const LayoutData*>(layout->_storage);
  auto* d = reinterpret_cast<ImageData*>(obj->_storage);
  *d = {};
  FormatInfo fi;
  if (!FormatOf(l->fmt, fi)) { Log("unsupported image format %d", int(l->fmt)); return; }
  d->fmt = l->fmt;
  d->w = l->w;
  d->h = l->h;
  d->internal = fi.internal;
  d->depth = fi.depth;
  glGenTextures(1, &d->tex);
  glBindTexture(GL_TEXTURE_2D, d->tex);
  glTexStorage2D(GL_TEXTURE_2D, 1, fi.internal, int(l->w), int(l->h));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  CheckGl("image create");
  // deko3d memory starts as whatever the pool held; GL's is zero: a fresh image is cleared to a
  // recognisable pattern only when CVHD_DK_GL_POISON is set (uninitialised-memory faults).
  if (std::getenv("CVHD_DK_GL_POISON") && !fi.compressed && !fi.depth && fi.format == GL_RGBA && fi.type == GL_UNSIGNED_BYTE) {
    std::vector<uint32_t> px(size_t(l->w) * l->h);
    for (size_t i = 0; i < px.size(); i++) px[i] = 0xFF00FFu | ((uint32_t(i * 2654435761u) >> 8) & 0xFF00u) << 0;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, int(l->w), int(l->h), GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  }
}
DkGpuAddr dkImageGetGpuAddr(DkImage const*) { return 0; }

void dkImageDescriptorInitialize(DkImageDescriptor* obj, DkImageView const* view, bool, bool) {
  auto* d = reinterpret_cast<ImageDescData*>(obj->_storage);
  d->image = reinterpret_cast<const ImageData*>(view->pImage->_storage);
  for (int i = 0; i < 4; i++) d->swizzle[i] = uint8_t(view->swizzle[i]);
}
void dkSamplerDescriptorInitialize(DkSamplerDescriptor* obj, DkSampler const* s) {
  auto* d = reinterpret_cast<SamplerDescData*>(obj->_storage);
  *d = {};
  d->min = uint8_t(s->minFilter);
  d->mag = uint8_t(s->magFilter);
  d->mip = uint8_t(s->mipFilter);
  for (int i = 0; i < 3; i++) d->wrap[i] = uint8_t(s->wrapMode[i]);
  for (int i = 0; i < 4; i++) d->border[i] = s->borderColor[i].value_ui;
}

}  // extern "C"
