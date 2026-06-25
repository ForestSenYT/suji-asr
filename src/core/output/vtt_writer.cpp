#include "core/output/vtt_writer.h"
#include "core/timestamp.h"
namespace suji {
std::string to_vtt(const Transcript& t) {
  std::string out = "WEBVTT\n\n";
  for (const auto& s : t.segments) {
    double end = (s.end > s.start) ? s.end : s.start + 2.0;
    out += format_vtt_time(s.start) + " --> " + format_vtt_time(end) + "\n";
    out += s.text + "\n\n";
  }
  return out;
}
}
