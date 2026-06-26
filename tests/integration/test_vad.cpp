#include "doctest/doctest.h"
#include "core/vad.h"
#include "core/media_decode.h"
#include "core/config.h"
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
