#include "doctest/doctest.h"
#include "core/output/writer_facade.h"
#include "core/pipeline.h"
#include "core/config.h"
#include <fstream>
using namespace suji;
TEST_CASE("write four outputs" * doctest::timeout(180)) {
  EngineConfig c; std::string md=SUJI_DEFAULT_MODELS_DIR; std::string m=md+
    "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG; c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt";
  c.vad_model=md+"/silero_vad.onnx";
  c.punct_model=md+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  Transcript t; std::string err;
  REQUIRE(transcribe_file(c, m+"test_wavs/0.wav", t, err));
  REQUIRE(write_outputs(t, "cli_e2e_out", c, "test"));
  for (auto ext : {".srt",".vtt",".json",".md"}) {
    std::ifstream in(std::string("cli_e2e_out")+ext, std::ios::binary);
    REQUIRE(in.good());
    std::string data((std::istreambuf_iterator<char>(in)),{});
    CHECK(data.size() > 0);
    CHECK_FALSE((unsigned char)data[0]==0xEF); // no BOM
  }
}
