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
