#include "doctest/doctest.h"
#include "core/pipeline.h"
#include "core/config.h"
using namespace suji;

static EngineConfig cfg() {
    EngineConfig c;
    std::string md = SUJI_DEFAULT_MODELS_DIR;
    c.ffmpeg_path = SUJI_DEFAULT_FFMPEG;
    std::string m = md + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
    c.asr_model  = m + "model.int8.onnx";
    c.tokens     = m + "tokens.txt";
    c.vad_model  = md + "/silero_vad.onnx";
    c.punct_model = md + "/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
    c.provider    = Provider::Cpu;
    c.num_threads = 4;
    return c;
}

TEST_CASE("end to end transcribe" * doctest::timeout(180)) {
    Transcript t;
    std::string err;
    auto c = cfg();
    std::string in = std::string(SUJI_DEFAULT_MODELS_DIR) +
        "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav";
    REQUIRE_MESSAGE(transcribe_file(c, in, t, err), err);
    REQUIRE(t.segments.size() >= 1);
    CHECK_FALSE(t.full_text.empty());
    // global timestamps must be monotonic non-decreasing
    for (size_t i = 1; i < t.segments.size(); ++i)
        CHECK(t.segments[i].start >= t.segments[i-1].start);
}
