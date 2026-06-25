#pragma once
#include "core/types.h"
#include "core/config.h"
#include <vector>
struct SherpaOnnxOfflineRecognizer;
namespace suji {
class Asr {
public:
  struct SegView { const float* samples; int n; };
  explicit Asr(const EngineConfig& cfg);
  ~Asr();
  Asr(const Asr&) = delete; Asr& operator=(const Asr&) = delete;
  bool ok() const { return rec_ != nullptr; }
  AsrResult transcribe(const float* samples, int n);
  std::vector<AsrResult> transcribe_batch(const std::vector<SegView>& segs);
private:
  const SherpaOnnxOfflineRecognizer* rec_ = nullptr;
};
}
