#include "core/output/vtt_writer.h"
#include "core/timestamp.h"
#include <string>

namespace suji {

// G5: same UTF-8 codepoint-aware wrap as srt_writer (self-contained copy).
static std::string wrap_codepoints(const std::string& text, int max_codepoints) {
  if (max_codepoints <= 0 || text.empty()) return text;

  std::string result;
  result.reserve(text.size() + text.size() / (static_cast<size_t>(max_codepoints) * 3) + 1);

  const unsigned char* p         = reinterpret_cast<const unsigned char*>(text.data());
  const unsigned char* end       = p + text.size();
  const unsigned char* line_start = p;
  int line_cp = 0;

  auto flush_line = [&](const unsigned char* upto, bool add_newline) {
    result.append(reinterpret_cast<const char*>(line_start),
                  static_cast<size_t>(upto - line_start));
    if (add_newline) result += '\n';
    line_start = upto;
    line_cp    = 0;
  };

  while (p < end) {
    unsigned char b = *p;
    int cp_len = (b < 0x80u) ? 1 : (b < 0xE0u) ? 2 : (b < 0xF0u) ? 3 : 4;
    const unsigned char* next = p + cp_len;
    if (next > end) next = end;

    bool is_break_char = (cp_len == 1 && (*p == ' ' || *p == '\n'));

    if (line_cp >= max_codepoints) {
      // Hard-break before this codepoint; skip leading ASCII spaces on new line
      flush_line(p, true);
      while (p < end && *p == ' ') { ++p; }
      line_start = p;
      continue;
    } else if (line_cp > 0 && line_cp >= max_codepoints - 2 && is_break_char) {
      flush_line(p, true);
      p         = next;
      line_start = p;
      continue;
    }

    ++line_cp;
    p = next;
  }

  if (line_start < end) {
    result.append(reinterpret_cast<const char*>(line_start),
                  static_cast<size_t>(end - line_start));
  }
  return result;
}

std::string to_vtt(const Transcript& t, const EngineConfig& cfg) {
  std::string out = "WEBVTT\n\n";
  for (const auto& s : t.segments) {
    // T7: use real segment end; +0.3 only as last resort when end<=start
    double end = (s.end > s.start) ? s.end : s.start + 0.3;
    out += format_vtt_time(s.start) + " --> " + format_vtt_time(end) + "\n";
    out += wrap_codepoints(s.text, cfg.srt_max_chars_per_line) + "\n\n";
  }
  return out;
}

} // namespace suji
