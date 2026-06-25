#include "core/resume.h"
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
namespace suji {

static bool exists_nonempty(const std::string& p) {
  std::error_code ec;
  return fs::exists(p, ec) && fs::file_size(p, ec) > 0;
}
static bool exists_any(const std::string& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}
static bool file_contains(const std::string& p, const std::string& needle) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::string data((std::istreambuf_iterator<char>(in)), {});
  return data.find(needle) != std::string::npos;
}

bool transcript_complete(const std::string& out_base, const EngineConfig& cfg) {
  // every enabled output must at least exist
  if (cfg.out_srt  && !exists_any(out_base + ".srt"))  return false;
  if (cfg.out_vtt  && !exists_any(out_base + ".vtt"))  return false;
  if (cfg.out_json && !exists_any(out_base + ".json")) return false;
  if (cfg.out_md   && !exists_any(out_base + ".md"))   return false;
  // content must be non-empty (segments>0), checked via the best available signal
  if (cfg.out_srt)  return exists_nonempty(out_base + ".srt");         // empty SRT == no segments
  if (cfg.out_json) return file_contains(out_base + ".json", "\"start\":"); // segment has a start
  if (cfg.out_md)   return file_contains(out_base + ".md",   "**[");   // segment anchor
  if (cfg.out_vtt)  return file_contains(out_base + ".vtt",  "-->");   // a cue
  return false; // nothing enabled -> never "complete"
}

}
