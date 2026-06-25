#pragma once
#include "core/config.h"
#include <string>
struct SherpaOnnxOfflinePunctuation;
namespace suji {
class Punctuator {
public:
  explicit Punctuator(const EngineConfig& cfg);
  ~Punctuator();
  Punctuator(const Punctuator&) = delete; Punctuator& operator=(const Punctuator&) = delete;
  bool ok() const { return punct_ != nullptr; }
  std::string add(const std::string& text);
private:
  const SherpaOnnxOfflinePunctuation* punct_ = nullptr;
};
}
