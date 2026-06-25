#pragma once
#include "core/types.h"
#include <string>
namespace suji {
bool decode_to_pcm(const std::string& ffmpeg, const std::string& input, AudioBuffer& out, std::string& err);
}
