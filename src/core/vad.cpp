#include "core/vad.h"
#include "sherpa-onnx/c-api/c-api.h"
#include <cstring>
namespace suji {
Vad::Vad(const EngineConfig& cfg) {
  window_ = cfg.vad_window;
  SherpaOnnxVadModelConfig c; std::memset(&c, 0, sizeof(c));
  c.silero_vad.model = cfg.vad_model.c_str();
  c.silero_vad.threshold = cfg.vad_threshold;
  c.silero_vad.min_silence_duration = cfg.vad_min_silence;
  c.silero_vad.min_speech_duration = cfg.vad_min_speech;
  c.silero_vad.max_speech_duration = cfg.vad_max_speech;
  c.silero_vad.window_size = cfg.vad_window;
  c.sample_rate = 16000; c.num_threads = 1; c.provider = "cpu"; c.debug = 0;
  vad_ = SherpaOnnxCreateVoiceActivityDetector(&c, 60.0f);
}
Vad::~Vad(){ if (vad_) SherpaOnnxDestroyVoiceActivityDetector(vad_); }
std::vector<SpeechSeg> Vad::segment(const AudioBuffer& audio, const CancelToken* cancel) {
  std::vector<SpeechSeg> out;
  if (!vad_) return out;
  const float* p = audio.samples.data();
  int64_t total = (int64_t)audio.samples.size();
  int64_t window_count = 0;
  for (int64_t i = 0; i + window_ <= total; i += window_, ++window_count) {
    // Check cancel every 64 windows (~32k samples ≈ 2 s of audio); cheap, prompt abort.
    if (cancel && (window_count % 64 == 0) && cancel->is_cancelled()) break;
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, p + i, window_);
    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
      const SherpaOnnxSpeechSegment* s = SherpaOnnxVoiceActivityDetectorFront(vad_);
      SpeechSeg seg; seg.start_sample = s->start; seg.samples.assign(s->samples, s->samples + s->n);
      out.push_back(std::move(seg));
      SherpaOnnxDestroySpeechSegment(s);
      SherpaOnnxVoiceActivityDetectorPop(vad_);
    }
  }
  SherpaOnnxVoiceActivityDetectorFlush(vad_);
  while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
    const SherpaOnnxSpeechSegment* s = SherpaOnnxVoiceActivityDetectorFront(vad_);
    SpeechSeg seg; seg.start_sample = s->start; seg.samples.assign(s->samples, s->samples + s->n);
    out.push_back(std::move(seg));
    SherpaOnnxDestroySpeechSegment(s);
    SherpaOnnxVoiceActivityDetectorPop(vad_);
  }
  return out;
}
}
