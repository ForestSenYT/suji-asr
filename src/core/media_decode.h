#pragma once
#include "core/types.h"
#include "core/cancel.h"
#include <string>
namespace suji {
bool decode_to_pcm(const std::string& ffmpeg, const std::string& input,
                   AudioBuffer& out, std::string& err,
                   const CancelToken* cancel = nullptr);

/// Run ffprobe to get the total duration of the media file.
/// Returns duration in seconds, or -1.0 on any failure. Never throws.
double probe_duration_seconds(const std::string& ffprobe, const std::string& input);
}
