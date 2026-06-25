#pragma once
#include "core/types.h"
#include "core/config.h"
struct SherpaOnnxOfflineRecognizer;
namespace suji {
class Asr {
public:
  explicit Asr(const EngineConfig& cfg);
  ~Asr();
  Asr(const Asr&) = delete; Asr& operator=(const Asr&) = delete;
  bool ok() const { return rec_ != nullptr; }
  AsrResult transcribe(const float* samples, int n);
private:
  const SherpaOnnxOfflineRecognizer* rec_ = nullptr;
};
}
