#include "core/output/md_writer.h"
#include "core/timestamp.h"
namespace suji {
static std::string hhmmss(double sec){ std::string s = format_srt_time(sec); return s.substr(0,8); } // HH:MM:SS
std::string to_markdown(const Transcript& t, const std::string& title){
  std::string o = "# " + title + "\n\n";
  for(const auto& s : t.segments) o += "**[" + hhmmss(s.start) + "]** " + s.text + "\n\n";
  return o;
}
}
