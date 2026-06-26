#pragma once
#include "core/types.h"
#include "core/config.h"
#include "core/cancel.h"
#include <vector>
struct SherpaOnnxVoiceActivityDetector;
namespace suji {
class Vad {
public:
  explicit Vad(const EngineConfig& cfg);
  ~Vad();
  Vad(const Vad&) = delete; Vad& operator=(const Vad&) = delete;
  bool ok() const { return vad_ != nullptr; }
  std::vector<SpeechSeg> segment(const AudioBuffer& audio,
                                 const CancelToken* cancel = nullptr);
private:
  const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
  int window_ = 512;
};
}
