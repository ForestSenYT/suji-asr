#include "doctest/doctest.h"
#include "core/vad.h"
#include "core/media_decode.h"
#include "core/config.h"
#include "core/cancel.h"
#include <vector>
using namespace suji;
static EngineConfig cfg(){ EngineConfig c; c.ffmpeg_path=SUJI_DEFAULT_FFMPEG;
  c.vad_model=std::string(SUJI_DEFAULT_MODELS_DIR)+"/silero_vad.onnx"; return c; }
TEST_CASE("vad yields segments" * doctest::timeout(60)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());
  auto segs = vad.segment(ab);
  REQUIRE(segs.size() >= 1);
  CHECK(segs[0].samples.size() > 0);
  CHECK(segs[0].start_sample >= 0);
}

// T11: one Vad is now built per producer thread and REUSED across files. This
// proves the reuse is safe: segmenting file B with a Vad that already segmented
// file A (reused) yields the SAME segments as a fresh Vad on file B. Vad::segment()
// calls Reset() internally, so prior-file LSTM state / queued segments never leak.
TEST_CASE("vad reuse across files matches fresh vad (T11)" * doctest::timeout(120)) {
  std::string w = std::string(SUJI_DEFAULT_MODELS_DIR)
                + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  AudioBuffer a0, a1; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path, w + "0.wav", a0, err));
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path, w + "1.wav", a1, err));

  // Reused detector: segment file 0, then file 1 on the SAME Vad.
  Vad reused(cfg()); REQUIRE(reused.ok());
  (void)reused.segment(a0);
  auto segs_reused = reused.segment(a1);

  // Fresh detector for file 1 (the per-file baseline).
  Vad fresh(cfg()); REQUIRE(fresh.ok());
  auto segs_fresh = fresh.segment(a1);

  REQUIRE(segs_reused.size() == segs_fresh.size());
  for (size_t i = 0; i < segs_fresh.size(); ++i) {
    CHECK(segs_reused[i].start_sample == segs_fresh[i].start_sample);
    CHECK(segs_reused[i].samples.size() == segs_fresh[i].samples.size());
  }
}

// P2: segment_stream emits the SAME segments (count + start_sample + sample count),
// in the same order, as segment() — since segment() is now built on segment_stream,
// this proves the streaming path and the collected path are one and the same.
TEST_CASE("vad segment_stream equals segment (P2)" * doctest::timeout(120)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());

  auto collected = vad.segment(ab);             // baseline (now built on segment_stream)
  REQUIRE(collected.size() >= 1);

  // Drive segment_stream directly and accumulate; callback must fire once per segment.
  std::vector<SpeechSeg> streamed;
  int callbacks = 0;
  vad.segment_stream(ab, [&](SpeechSeg&& s){ ++callbacks; streamed.push_back(std::move(s)); return true; });

  CHECK(callbacks == (int)collected.size());           // one callback per emitted segment
  REQUIRE(streamed.size() == collected.size());
  for (size_t i = 0; i < collected.size(); ++i) {
    CHECK(streamed[i].start_sample == collected[i].start_sample);
    CHECK(streamed[i].samples.size() == collected[i].samples.size());
  }
}

// P2: returning false from the callback stops emission early. With >=2 segments,
// stopping after the first means exactly one callback fires.
TEST_CASE("vad segment_stream early-stop on callback false (P2)" * doctest::timeout(120)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());
  REQUIRE(vad.segment(ab).size() >= 2);   // file has multiple segments

  int callbacks = 0;
  vad.segment_stream(ab, [&](SpeechSeg&&){ ++callbacks; return false; });  // stop after first
  CHECK(callbacks == 1);                  // emission stopped immediately on false
}

// P2: a cancelled token aborts segment_stream just like segment(); no callbacks
// after cancel (pre-cancelled token yields zero segments).
TEST_CASE("vad segment_stream honors cancel (P2)" * doctest::timeout(120)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());
  CancelToken cancel; cancel.cancel();    // already cancelled
  int callbacks = 0;
  vad.segment_stream(ab, [&](SpeechSeg&&){ ++callbacks; return true; }, &cancel);
  CHECK(callbacks == 0);                   // cancel checked at loop entry -> no emission
}

// P3 (streaming decode) — KEY CORRECTNESS: feeding the SAME audio to the incremental
// accept()/finish() API in DIFFERENT chunk sizes must yield byte-for-byte IDENTICAL
// segments (count + start_sample + sample count), and those must equal the old
// segment()/segment_stream() output. Window-aligned feeding buffers the remainder so the
// AcceptWaveform call sequence is independent of how the input is chopped up.
TEST_CASE("vad accept is chunk-invariant and equals segment (P3)" * doctest::timeout(180)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());

  // Baseline: the whole-buffer collected path (segment() -> segment_stream()).
  auto baseline = vad.segment(ab);
  REQUIRE(baseline.size() >= 2);   // multiple segments make the comparison meaningful

  // Helper: drive accept() with a fixed chunk size, then finish(); collect segments.
  auto run_chunked = [&](int chunk) {
    std::vector<SpeechSeg> out;
    auto sink = [&](SpeechSeg&& s){ out.push_back(std::move(s)); return true; };
    vad.reset();
    const float* p = ab.samples.data();
    int total = (int)ab.samples.size();
    for (int i = 0; i < total; i += chunk) {
      int n = (i + chunk <= total) ? chunk : (total - i);
      REQUIRE(vad.accept(p + i, n, sink));   // no cancel -> never stops early
    }
    vad.finish(sink);
    return out;
  };

  // Compare a chunked run against the baseline (identical count + start + sample count).
  auto same_as_baseline = [&](const std::vector<SpeechSeg>& got){
    REQUIRE(got.size() == baseline.size());
    for (size_t i = 0; i < baseline.size(); ++i) {
      CHECK(got[i].start_sample == baseline[i].start_sample);
      CHECK(got[i].samples.size() == baseline[i].samples.size());
    }
  };

  // One big call (whole buffer), then progressively pathological chunk sizes:
  // tiny (< window), window-aligned, window+1 (forces leftover carry), an odd prime,
  // and a large multi-window chunk. ALL must match the baseline exactly.
  same_as_baseline(run_chunked((int)ab.samples.size()));  // single accept()
  same_as_baseline(run_chunked(1));                       // one sample at a time
  same_as_baseline(run_chunked(100));                     // < window_ (512)
  same_as_baseline(run_chunked(512));                     // window-aligned
  same_as_baseline(run_chunked(513));                     // window+1 (leftover carry)
  same_as_baseline(run_chunked(997));                     // odd prime (never window-aligned)
  same_as_baseline(run_chunked(40000));                   // large multi-window chunk
}
