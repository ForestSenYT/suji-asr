#pragma once
#include "core/types.h"
#include "core/cancel.h"
#include "core/vad.h"
#include <functional>
#include <string>
namespace suji {
bool decode_to_pcm(const std::string& ffmpeg, const std::string& input,
                   AudioBuffer& out, std::string& err,
                   const CancelToken* cancel = nullptr);

/// P3: STREAMING decode + VAD. Spawns ffmpeg exactly like decode_to_pcm (UTF-16 path,
/// CREATE_NO_WINDOW, stderr->NUL, cancel-aware ReadFile loop, TerminateProcess on cancel)
/// but instead of accumulating the whole file's PCM it feeds each read INCREMENTALLY into
/// `vad` via vad.accept(), emitting speech segments to `on_seg` the instant they are found,
/// then vad.finish() at EOF. Never holds more than one read buffer + the VAD's leftover, so
/// a multi-hour file starts transcribing in seconds with no whole-file memory spike.
/// `vad.reset()` is called once at start (the function owns the stream lifecycle).
/// Segment start_sample is the running sample offset from stream start (global, no offset
/// math needed since the whole stream is fed continuously).
/// Returns false (err set) on: spawn failure, zero audio produced, or cancel (err="cancelled").
/// on_seg returns false => stop early (cancel / backpressure); ffmpeg is then terminated.
/// If `total_samples_out` is non-null it receives the total decoded sample count (full file
/// duration incl. silence) so callers can keep throughput metrics without buffering the file.
bool decode_vad_stream(const std::string& ffmpeg, const std::string& input, Vad& vad,
                       const std::function<bool(SpeechSeg&&)>& on_seg, std::string& err,
                       const CancelToken* cancel = nullptr,
                       int64_t* total_samples_out = nullptr);

/// Run ffprobe to get the total duration of the media file.
/// Returns duration in seconds, or -1.0 on any failure. Never throws.
double probe_duration_seconds(const std::string& ffprobe, const std::string& input);
}
