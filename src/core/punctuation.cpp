#include "core/punctuation.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <cstring>
namespace suji {
Punctuator::Punctuator(const EngineConfig& cfg) {
  if (cfg.punct_model.empty()) return;
  SherpaOnnxOfflinePunctuationConfig c; std::memset(&c, 0, sizeof(c));
  c.model.ct_transformer = cfg.punct_model.c_str();
  c.model.num_threads = cfg.punct_threads; c.model.provider = cfg.punct_provider.c_str(); c.model.debug = 0;
  punct_ = SherpaOnnxCreateOfflinePunctuation(&c);
}
Punctuator::~Punctuator(){ if (punct_) SherpaOnnxDestroyOfflinePunctuation(punct_); }
std::string Punctuator::add(const std::string& text) {
  if (!punct_ || text.empty()) return text;
  const char* res = SherpaOfflinePunctuationAddPunct(punct_, text.c_str());
  std::string out = res ? res : text;
  if (res) SherpaOfflinePunctuationFreeText(res);
  return out;
}
}
