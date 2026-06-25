#include "core/timestamp.h"
#include <cstdio>
#include <cmath>
namespace suji {
static std::string fmt(double seconds, char ms_sep) {
  if (seconds < 0) seconds = 0;
  long long total_ms = (long long)std::llround(seconds * 1000.0);
  int ms = (int)(total_ms % 1000); long long s = total_ms / 1000;
  int sec = (int)(s % 60); long long m = s / 60;
  int minute = (int)(m % 60); int hour = (int)(m / 60);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%03d", hour, minute, sec, ms_sep, ms);
  return std::string(buf);
}
std::string format_srt_time(double s){ return fmt(s, ','); }
std::string format_vtt_time(double s){ return fmt(s, '.'); }
}
