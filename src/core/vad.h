#pragma once
#include "core/types.h"
#include "core/config.h"
#include "core/cancel.h"
#include <vector>
#include <functional>
struct SherpaOnnxVoiceActivityDetector;
namespace suji {
class Vad {
public:
  explicit Vad(const EngineConfig& cfg);
  ~Vad();
  Vad(const Vad&) = delete; Vad& operator=(const Vad&) = delete;
  bool ok() const { return vad_ != nullptr; }

  // P3 (streaming decode): INCREMENTAL streaming API. The three calls below let a
  // producer feed PCM into the VAD as ffmpeg decodes it, never holding the whole file.
  //   reset()  -> clear detector state for a new file (LSTM state + queued segments).
  //   accept() -> append `data` to an internal leftover buffer and feed sherpa
  //               AcceptWaveform in EXACTLY window_-sized pieces (buffering the
  //               remainder < window_ for the next call), popping ready segments to
  //               on_seg after each window. Feeding window_-sized pieces guarantees
  //               AcceptWaveform sees the SAME sequence regardless of how `data` is
  //               chunked, so segment identity is independent of chunk boundaries.
  //               Returns false if it stopped early (cancel fired, or on_seg returned
  //               false) so the caller can skip finish() — matching the old loop which
  //               did NOT Flush on early stop.
  //   finish() -> Flush + drain remaining segments (matches segment_stream's tail).
  // on_seg returns false => stop early (cancel / backpressure). `cancel` is checked
  // every ~64 windows inside accept(), like the old segment loop.
  void reset();
  bool accept(const float* data, int n,
              const std::function<bool(SpeechSeg&&)>& on_seg,
              const CancelToken* cancel = nullptr);
  void finish(const std::function<bool(SpeechSeg&&)>& on_seg);

  // P2: STREAMING segmentation over a whole in-memory buffer. Invokes on_seg(seg) for
  // each speech segment the instant the VAD emits it (in time order), so a consumer can
  // start transcribing while VAD is still running over the rest of the buffer. on_seg
  // returns false => stop early (cancel / backpressure). Now a thin wrapper over the
  // incremental API (reset + one accept + finish) so there is ONE segmentation path.
  void segment_stream(const AudioBuffer& audio,
                      const std::function<bool(SpeechSeg&&)>& on_seg,
                      const CancelToken* cancel = nullptr);

  // Collect-all convenience wrapper built ON TOP of segment_stream (single code path):
  // returns every speech segment in time order. Used by suji_cli / pipeline.cpp.
  std::vector<SpeechSeg> segment(const AudioBuffer& audio,
                                 const CancelToken* cancel = nullptr);
private:
  // Drain all currently-ready segments to on_seg. Returns false if a callback asked to
  // stop (cancel/backpressure) so callers can break out of their loop promptly.
  bool drain(const std::function<bool(SpeechSeg&&)>& on_seg);

  const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
  int window_ = 512;
  // Leftover PCM (< window_ samples) carried across accept() calls so AcceptWaveform is
  // always fed in exact window_-sized pieces. reset() clears it for each new stream.
  std::vector<float> leftover_;
  // Window counter (across the whole stream) for the every-64-windows cancel check,
  // matching the old per-call loop cadence. reset() zeroes it.
  int64_t window_count_ = 0;
};
}
