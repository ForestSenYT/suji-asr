#pragma once
#include <string>
namespace suji {
std::string format_srt_time(double seconds); // HH:MM:SS,mmm
std::string format_vtt_time(double seconds); // HH:MM:SS.mmm
}
