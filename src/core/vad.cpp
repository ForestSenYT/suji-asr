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
void Vad::segment_stream(const AudioBuffer& audio,
                         const std::function<bool(SpeechSeg&&)>& on_seg,
                         const CancelToken* cancel) {
  if (!vad_) return;
  // T11: a single Vad is reused across many files (one per producer thread). Reset the
  // detector at the start of every call so each file is segmented as a fresh stream —
  // clears the Silero LSTM hidden state AND any queued segments from a prior file, so
  // every call is fully self-contained regardless of what came before.
  SherpaOnnxVoiceActivityDetectorReset(vad_);
  const float* p = audio.samples.data();
  int64_t total = (int64_t)audio.samples.size();
  int64_t window_count = 0;
  // Drain all currently-ready segments to on_seg; return false if a callback asked to
  // stop (cancel/backpressure) so the caller can break out of the window loop promptly.
  auto drain = [&]() -> bool {
    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
      const SherpaOnnxSpeechSegment* s = SherpaOnnxVoiceActivityDetectorFront(vad_);
      SpeechSeg seg; seg.start_sample = s->start; seg.samples.assign(s->samples, s->samples + s->n);
      SherpaOnnxDestroySpeechSegment(s);
      SherpaOnnxVoiceActivityDetectorPop(vad_);
      // Emit AFTER popping so the detector's internal queue is already advanced; if the
      // callback stops us, no segment is dropped or double-emitted.
      if (!on_seg(std::move(seg))) return false;
    }
    return true;
  };
  for (int64_t i = 0; i + window_ <= total; i += window_, ++window_count) {
    // Check cancel every 64 windows (~32k samples ≈ 2 s of audio); cheap, prompt abort.
    if (cancel && (window_count % 64 == 0) && cancel->is_cancelled()) return;
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, p + i, window_);
    if (!drain()) return;   // stream each ready segment immediately; stop if asked
  }
  SherpaOnnxVoiceActivityDetectorFlush(vad_);
  drain();                  // final tail segments after flush
}

std::vector<SpeechSeg> Vad::segment(const AudioBuffer& audio, const CancelToken* cancel) {
  // Built ON TOP of segment_stream so there is ONE segmentation code path: collect every
  // emitted segment into a vector. Behaviour (Reset, cancel, segment identity) is identical.
  std::vector<SpeechSeg> out;
  segment_stream(audio, [&](SpeechSeg&& s){ out.push_back(std::move(s)); return true; }, cancel);
  return out;
}
}
