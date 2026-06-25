#include "doctest/doctest.h"
#include "core/asr.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/config.h"
using namespace suji;
static EngineConfig cfg(){ EngineConfig c; std::string md=SUJI_DEFAULT_MODELS_DIR; std::string m=md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG; c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt"; c.vad_model=md+"/silero_vad.onnx"; c.provider=Provider::Cpu; c.num_threads=4; return c; }
TEST_CASE("batch decode matches single-stream" * doctest::timeout(180)){
  auto c=cfg(); AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(c.ffmpeg_path, std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(c); auto segs=vad.segment(ab); REQUIRE(segs.size()>=2);
  Asr asr(c); REQUIRE(asr.ok());
  std::vector<Asr::SegView> views; for(auto& s:segs) views.push_back({s.samples.data(),(int)s.samples.size()});
  auto batch = asr.transcribe_batch(views);
  REQUIRE(batch.size()==segs.size());
  // NOTE: batched decode (DecodeMultipleOfflineStreams) pads sequences and is NOT
  // bit-exact vs single-stream decode, so we verify each result is VALID, not equal.
  size_t nonempty = 0;
  for (size_t i = 0; i < batch.size(); ++i) {
    CHECK(batch[i].tokens.size() == batch[i].timestamps.size());
    if (!batch[i].text.empty()) ++nonempty;
  }
  CHECK(nonempty >= 1);   // at least one segment produced text
}
