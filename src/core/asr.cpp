#include "core/asr.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <cstring>
namespace suji {
Asr::Asr(const EngineConfig& cfg) {
  SherpaOnnxOfflineRecognizerConfig c; std::memset(&c, 0, sizeof(c));
  c.feat_config.sample_rate = 16000;
  c.feat_config.feature_dim = 80;
  c.model_config.fire_red_asr_ctc.model = cfg.asr_model.c_str();
  c.model_config.tokens = cfg.tokens.c_str();
  c.model_config.num_threads = (cfg.provider == Provider::Cuda) ? 1 : cfg.num_threads;
  c.model_config.provider = provider_str(cfg.provider);
  c.model_config.debug = 0;
  c.decoding_method = "greedy_search";
  if (!cfg.rule_fsts.empty()) c.rule_fsts = cfg.rule_fsts.c_str();
  rec_ = SherpaOnnxCreateOfflineRecognizer(&c);
}
Asr::~Asr(){ if (rec_) SherpaOnnxDestroyOfflineRecognizer(rec_); }
AsrResult Asr::transcribe(const float* samples, int n) {
  AsrResult out;
  if (!rec_) return out;
  const SherpaOnnxOfflineStream* st = SherpaOnnxCreateOfflineStream(rec_);
  SherpaOnnxAcceptWaveformOffline(st, 16000, samples, n);
  SherpaOnnxDecodeOfflineStream(rec_, st);
  const SherpaOnnxOfflineRecognizerResult* r = SherpaOnnxGetOfflineStreamResult(st);
  if (r) {
    if (r->text) out.text = r->text;
    int count = r->count;
    for (int i = 0; i < count; ++i) {
      out.tokens.push_back(r->tokens_arr && r->tokens_arr[i] ? r->tokens_arr[i] : "");
      out.timestamps.push_back(r->timestamps ? (double)r->timestamps[i] : 0.0);
    }
    SherpaOnnxDestroyOfflineRecognizerResult(r);
  }
  SherpaOnnxDestroyOfflineStream(st);
  return out;
}
std::vector<AsrResult> Asr::transcribe_batch(const std::vector<SegView>& segs){
  std::vector<AsrResult> out(segs.size());
  if(!rec_ || segs.empty()) return out;
  std::vector<const SherpaOnnxOfflineStream*> streams;
  streams.reserve(segs.size());
  for(auto& s : segs){
    const SherpaOnnxOfflineStream* st = SherpaOnnxCreateOfflineStream(rec_);
    SherpaOnnxAcceptWaveformOffline(st, 16000, s.samples, s.n);
    streams.push_back(st);
  }
  SherpaOnnxDecodeMultipleOfflineStreams(rec_, streams.data(), (int)streams.size());
  for(size_t i=0;i<streams.size();++i){
    const SherpaOnnxOfflineRecognizerResult* r = SherpaOnnxGetOfflineStreamResult(streams[i]);
    if(r){
      if(r->text) out[i].text=r->text;
      int count=r->count;
      for(int k=0;k<count;++k){
        out[i].tokens.push_back(r->tokens_arr && r->tokens_arr[k] ? r->tokens_arr[k] : "");
        out[i].timestamps.push_back(r->timestamps ? (double)r->timestamps[k] : 0.0);
      }
      SherpaOnnxDestroyOfflineRecognizerResult(r);
    }
    SherpaOnnxDestroyOfflineStream(streams[i]);
  }
  return out;
}
}
