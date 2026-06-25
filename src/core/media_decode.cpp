#include "core/media_decode.h"
#include <cstdio>
#include <vector>
namespace suji {
bool decode_to_pcm(const std::string& ffmpeg, const std::string& input, AudioBuffer& out, std::string& err) {
  // cmd.exe rules: outer quotes wrap the entire command when paths contain spaces
  std::string cmd = "\"\"" + ffmpeg + "\" -nostdin -loglevel error -i \"" + input +
                    "\" -vn -ar 16000 -ac 1 -f f32le -\"";
  FILE* pipe = _popen(cmd.c_str(), "rb");
  if (!pipe) { err = "failed to start ffmpeg"; return false; }
  out.samples.clear(); out.sample_rate = 16000;
  std::vector<float> buf(65536);
  size_t n;
  while ((n = std::fread(buf.data(), sizeof(float), buf.size(), pipe)) > 0)
    out.samples.insert(out.samples.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
  int rc = _pclose(pipe);
  if (out.samples.empty()) { err = "ffmpeg produced no audio (rc=" + std::to_string(rc) + ")"; return false; }
  return true;
}
}
