#include "doctest/doctest.h"
#include "core/media_decode.h"
#include <string>
using namespace suji;
static std::string ffmpeg(){ return SUJI_DEFAULT_FFMPEG; }
static std::string wav0(){ return std::string(SUJI_DEFAULT_MODELS_DIR) + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav"; }
TEST_CASE("decode wav to 16k mono pcm" * doctest::timeout(60)) {
  AudioBuffer ab; std::string err;
  REQUIRE_MESSAGE(decode_to_pcm(ffmpeg(), wav0(), ab, err), err);
  CHECK(ab.sample_rate == 16000);
  CHECK(ab.samples.size() > 120000);   // >7.5s
  CHECK(ab.samples.size() < 200000);   // <12.5s
}
TEST_CASE("decode missing file fails") {
  AudioBuffer ab; std::string err;
  CHECK_FALSE(decode_to_pcm(ffmpeg(), "no_such_file.wav", ab, err));
}
