// For each extracted shader (<dir>/shader_<hash>.ucode.be.<vert|frag>, big-endian microcode), writes the same pair of
// files the SDK writes when it dumps a shader at run time: shader_<hash>.ucode.<stage> (disassembly, the input of
// tools/scripts/xenos2glsl.py) and shader_<hash>.ucode.bin.<stage>, using the SDK's own Shader::AnalyzeUcode and
// DumpUcode (librexgpu-xenos from the host SDK build).
//   disasm_dump <in dir> <out dir>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

// The one setting of the SDK's graphics flags (src/graphics/flags.cpp) the shader code reads.
REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "Path to dump shaders to");

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <in dir> <out dir>\n", argv[0]); return 2; }
  namespace fs = std::filesystem;
  fs::create_directories(argv[2]);
  size_t ok = 0, bad = 0;
  for (const auto& e : fs::directory_iterator(argv[1])) {
    const std::string name = e.path().filename().string();  // shader_<16 hex>.ucode.be.<stage>
    if (name.rfind("shader_", 0) != 0 || name.find(".ucode.be.") == std::string::npos) continue;
    const bool vert = name.size() > 5 && name.substr(name.size() - 5) == ".vert";
    const uint64_t hash = std::stoull(name.substr(7, 16), nullptr, 16);
    std::ifstream f(e.path(), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() % 4) { bad++; continue; }
    // Each shader in a child process: the scan's rare false positives are not real microcode and can crash the
    // SDK's analysis; such a shader is skipped (and counted) instead of ending the run.
    const pid_t pid = fork();
    if (pid == 0) {
      rex::graphics::Shader shader(vert ? rex::graphics::xenos::ShaderType::kVertex : rex::graphics::xenos::ShaderType::kPixel,
                                   hash, reinterpret_cast<const uint32_t*>(bytes.data()), bytes.size() / 4);
      rex::string::StringBuffer disasm;
      shader.AnalyzeUcode(disasm);
      if (!shader.is_ucode_analyzed()) _exit(1);
      shader.DumpUcode(argv[2]);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) ok++; else bad++;
  }
  std::fprintf(stderr, "%zu dumped, %zu failed\n", ok, bad);
  return 0;
}
