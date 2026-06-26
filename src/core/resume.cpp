#include "core/resume.h"
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;
namespace suji {

static bool exists_any(const std::string& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}
static std::string read_file(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

// Check whether 'data' ends with a proper blank-line terminator (\n\n),
// tolerating trailing \r bytes from \r\n line endings.
static bool ends_with_double_newline(const std::string& data) {
  size_t end = data.size();
  while (end > 0 && data[end - 1] == '\r') --end;
  return end >= 2 && data[end - 1] == '\n' && data[end - 2] == '\n';
}

// --- Per-format completeness checks (operate on in-memory data) ---

// JSON: must contain a segment marker AND close with '}' (no truncation).
static bool json_complete(const std::string& data) {
  if (data.empty()) return false;
  if (data.find("\"start\":") == std::string::npos) return false;
  // Last non-whitespace char must be '}' (properly closed JSON object)
  size_t end = data.size();
  while (end > 0 && (data[end-1] == ' ' || data[end-1] == '\t' ||
                      data[end-1] == '\r' || data[end-1] == '\n'))
    --end;
  return end > 0 && data[end - 1] == '}';
}

// SRT: must have at least one complete cue:
//   <number>\n<start --> end>\n<text line>\n\n
// A truncated file may have the arrow line but no text, or no trailing blank line.
static bool srt_complete(const std::string& data) {
  if (data.empty()) return false;
  size_t arrow = data.find(" --> ");
  if (arrow == std::string::npos) return false;
  // After the arrow line there must be at least one non-empty text line
  size_t nl = data.find('\n', arrow);
  if (nl == std::string::npos) return false;        // no newline after arrow
  size_t text_start = nl + 1;
  if (text_start >= data.size()) return false;      // nothing after arrow line
  if (data[text_start] == '\n' || data[text_start] == '\r') return false; // empty text
  // File must end with a blank line (cue block terminator)
  return ends_with_double_newline(data);
}

// VTT: same structure as SRT, but must also start with "WEBVTT".
static bool vtt_complete(const std::string& data) {
  if (data.rfind("WEBVTT", 0) != 0) return false;
  return srt_complete(data);
}

// MD: must have at least one segment marker **[ and end with \n\n.
static bool md_complete(const std::string& data) {
  if (data.empty()) return false;
  if (data.find("**[") == std::string::npos) return false;
  return ends_with_double_newline(data);
}

bool transcript_complete(const std::string& out_base, const EngineConfig& cfg) {
  // every enabled output must at least exist
  if (cfg.out_srt  && !exists_any(out_base + ".srt"))  return false;
  if (cfg.out_vtt  && !exists_any(out_base + ".vtt"))  return false;
  if (cfg.out_json && !exists_any(out_base + ".json")) return false;
  if (cfg.out_md   && !exists_any(out_base + ".md"))   return false;

  // Robust completeness: each enabled format must pass a structural check
  // that a truncated file would fail.
  if (cfg.out_srt  && !srt_complete(read_file(out_base + ".srt")))   return false;
  if (cfg.out_vtt  && !vtt_complete(read_file(out_base + ".vtt")))   return false;
  if (cfg.out_json && !json_complete(read_file(out_base + ".json"))) return false;
  if (cfg.out_md   && !md_complete(read_file(out_base + ".md")))     return false;

  // At least one format must be enabled (nothing enabled -> never "complete")
  return cfg.out_srt || cfg.out_vtt || cfg.out_json || cfg.out_md;
}

}
