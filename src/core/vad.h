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

  // P2: STREAMING segmentation. Invokes on_seg(seg) for each speech segment the
  // instant the VAD emits it (in time order), so a consumer can start transcribing
  // while VAD is still running over the rest of the file. on_seg returns false =>
  // stop early (cancel / backpressure). Honors `cancel` like segment() (checked every
  // ~64 windows). Resets the detector at entry so each call is a self-contained stream.
  void segment_stream(const AudioBuffer& audio,
                      const std::function<bool(SpeechSeg&&)>& on_seg,
                      const CancelToken* cancel = nullptr);

  // Collect-all convenience wrapper built ON TOP of segment_stream (single code path):
  // returns every speech segment in time order. Used by suji_cli / pipeline.cpp.
  std::vector<SpeechSeg> segment(const AudioBuffer& audio,
                                 const CancelToken* cancel = nullptr);
private:
  const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
  int window_ = 512;
};
}
