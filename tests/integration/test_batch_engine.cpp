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
  int last_done = 0;
  auto res = transcribe_batch_files(inputs, cfg(), tune, [&](const BatchProgress& b){ last_done = b.files_done; });
  REQUIRE(res.size()==3);
  CHECK(res[0].ok); CHECK_FALSE(res[1].ok); CHECK(res[2].ok);   // bad file isolated
  CHECK(res[1].err.size()>0);
  CHECK(last_done == 3);   // progress counts ALL files incl. the failed one
}
TEST_CASE("live progress fires during transcription (not only at finalize)" * doctest::timeout(300)){
  // 0.wav has multiple VAD segments -> multiple consumer batches -> consumer cb fires > 1 time
  // finalize loop fires exactly 1 time (one file). So cb_count > 1 proves live emission.
  std::string w=md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  std::vector<std::string> inputs={ w+"0.wav" };
  AutoTune tune; tune.provider=Provider::Cpu; tune.batch=1; tune.in_flight_files=1; tune.num_threads=4;
  // batch=1 forces one consumer cb call per VAD segment batch, maximizing cb count
  int cb_count = 0;
  auto res = transcribe_batch_files(inputs, cfg(), tune, [&](const BatchProgress& b){
    (void)b;
    ++cb_count;
  });
  REQUIRE(res.size()==1);
  CHECK(res[0].ok);
  // cb_count must exceed inputs.size() (1), proving callback fired during transcription
  // (finalize loop fires exactly 1 time; consumer fires once per VAD batch)
  CHECK(cb_count > (int)inputs.size());
}
