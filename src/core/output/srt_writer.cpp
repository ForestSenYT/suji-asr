#include "core/output/srt_writer.h"
#include "core/timestamp.h"
namespace suji {
std::string to_srt(const Transcript& t) {
  std::string out;
  int idx = 1;
  for (const auto& s : t.segments) {
    double end = (s.end > s.start) ? s.end : s.start + 2.0; // 防 0 时长
    out += std::to_string(idx++) + "\n";
    out += format_srt_time(s.start) + " --> " + format_srt_time(end) + "\n";
    out += s.text + "\n\n";
  }
  return out;
}
}
