#include "doctest/doctest.h"
#include "core/asr.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/config.h"
using namespace suji;
static EngineConfig cfg(){ EngineConfig c; std::string md=SUJI_DEFAULT_MODELS_DIR;
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG;
  c.asr_model=md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/model.int8.onnx";
  c.tokens   =md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/tokens.txt";
  c.vad_model=md+"/silero_vad.onnx"; c.provider=Provider::Cpu; c.num_threads=4; return c; }
TEST_CASE("asr transcribe a segment" * doctest::timeout(120)) {
  AudioBuffer ab; std::string err; auto c=cfg();
  REQUIRE(decode_to_pcm(c.ffmpeg_path, std::string(SUJI_DEFAULT_MODELS_DIR)+
    "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Asr asr(c); REQUIRE(asr.ok());
  auto r = asr.transcribe(ab.samples.data(), (int)ab.samples.size());
  CHECK_FALSE(r.text.empty());
  CHECK(r.tokens.size() == r.timestamps.size());
  CHECK(r.timestamps.size() >= 1);
}
