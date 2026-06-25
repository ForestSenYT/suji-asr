#include "doctest/doctest.h"
#include "core/batch_engine.h"
#include "core/config.h"
#include <string>
using namespace suji;
static std::string md(){ return SUJI_DEFAULT_MODELS_DIR; }
static EngineConfig cfg(){ EngineConfig c; std::string m=md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG; c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt"; c.vad_model=md()+"/silero_vad.onnx";
  c.punct_model=md()+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx"; c.provider=Provider::Cpu; c.num_threads=4; return c; }
TEST_CASE("batch transcribe multiple files (CPU)" * doctest::timeout(300)){
  std::string w=md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  std::vector<std::string> inputs={ w+"0.wav", w+"1.wav", w+"2.wav" };
  AutoTune tune; tune.provider=Provider::Cpu; tune.batch=4; tune.in_flight_files=2; tune.num_threads=4;
  auto res = transcribe_batch_files(inputs, cfg(), tune);
  REQUIRE(res.size()==3);
  for(auto& r : res){ CHECK(r.ok); CHECK_FALSE(r.transcript.full_text.empty()); }
}
TEST_CASE("batch isolates a bad file" * doctest::timeout(300)){
  std::string w=md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  std::vector<std::string> inputs={ w+"0.wav", "no_such_file.wav", w+"1.wav" };
  AutoTune tune; tune.provider=Provider::Cpu; tune.batch=4; tune.in_flight_files=2; tune.num_threads=4;
  auto res = transcribe_batch_files(inputs, cfg(), tune);
  REQUIRE(res.size()==3);
  CHECK(res[0].ok); CHECK_FALSE(res[1].ok); CHECK(res[2].ok);   // bad file isolated
  CHECK(res[1].err.size()>0);
}
