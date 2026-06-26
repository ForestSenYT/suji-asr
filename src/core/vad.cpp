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

bool Vad::drain(const std::function<bool(SpeechSeg&&)>& on_seg) {
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
}

void Vad::reset() {
  if (!vad_) return;
  // T11: a single Vad is reused across many files (one per producer thread). Reset the
  // detector at the start of every stream so each file is segmented fresh — clears the
  // Silero LSTM hidden state AND any queued segments from a prior file. Also clear our
  // leftover PCM and window counter so the new stream is fully self-contained.
  SherpaOnnxVoiceActivityDetectorReset(vad_);
  leftover_.clear();
  window_count_ = 0;
}

bool Vad::accept(const float* data, int n,
                 const std::function<bool(SpeechSeg&&)>& on_seg,
                 const CancelToken* cancel) {
  if (!vad_) return true;
  if (n > 0) {
    // Append the new PCM to whatever partial window we carried from the previous call.
    leftover_.insert(leftover_.end(), data, data + n);
  }
  // Feed sherpa AcceptWaveform in EXACTLY window_-sized pieces. Buffering the remainder
  // (< window_) for the next call makes the AcceptWaveform call sequence identical
  // regardless of how `data` was chunked => segment identity is chunk-invariant.
  size_t off = 0;
  const size_t w = (size_t)window_;
  while (leftover_.size() - off >= w) {
    // Check cancel every 64 windows (~32k samples ≈ 2 s of audio); cheap, prompt abort.
    if (cancel && (window_count_ % 64 == 0) && cancel->is_cancelled()) {
      // Drop everything not yet fed; the stream is being abandoned. Caller skips finish().
      leftover_.clear();
      return false;
    }
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, leftover_.data() + off, window_);
    ++window_count_;
    off += w;
    if (!drain(on_seg)) { leftover_.clear(); return false; }  // on_seg asked to stop
  }
  // Keep only the unfed remainder (< window_) for the next accept().
  if (off > 0) leftover_.erase(leftover_.begin(), leftover_.begin() + off);
  return true;
}

void Vad::finish(const std::function<bool(SpeechSeg&&)>& on_seg) {
  if (!vad_) return;
  // Match the old segment loop exactly: the trailing partial window (< window_ samples)
  // is NOT fed to AcceptWaveform — Flush handles the tail. Feeding it would change the
  // emitted segments vs. today, so we drop it (segment output stays IDENTICAL).
  SherpaOnnxVoiceActivityDetectorFlush(vad_);
  // Honor the interrupt: on cancel during the clean-EOF flush, on_seg returns false and we
  // must STOP emitting tail segments (mirrors accept()'s drain() check ~66) instead of
  // invoking on_seg for every remaining tail segment.
  if(!drain(on_seg)){ leftover_.clear(); return; }   // final tail segments after flush
  leftover_.clear();
}

void Vad::segment_stream(const AudioBuffer& audio,
                         const std::function<bool(SpeechSeg&&)>& on_seg,
                         const CancelToken* cancel) {
  // ONE segmentation path: reset, feed the whole buffer in one accept(), finish.
  // Identical AcceptWaveform sequence and tail handling as the prior loop. If accept()
  // stopped early (cancel / on_seg returned false), skip finish() so we DON'T Flush —
  // exactly what the old loop did on early `return`.
  reset();
  if (accept(audio.samples.data(), (int)audio.samples.size(), on_seg, cancel))
    finish(on_seg);
}

std::vector<SpeechSeg> Vad::segment(const AudioBuffer& audio, const CancelToken* cancel) {
  // Built ON TOP of segment_stream so there is ONE segmentation code path: collect every
  // emitted segment into a vector. Behaviour (Reset, cancel, segment identity) is identical.
  std::vector<SpeechSeg> out;
  segment_stream(audio, [&](SpeechSeg&& s){ out.push_back(std::move(s)); return true; }, cancel);
  return out;
}
}
