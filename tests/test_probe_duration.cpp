#include "doctest/doctest.h"
#include "core/media_decode.h"
#include "core/paths.h"
#include <string>

using namespace suji;

TEST_CASE("probe_duration_seconds returns plausible duration for test wav") {
    // The test wav lives at models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav
    // ffprobe_path() resolves to the dev-tree vendor ffprobe.exe
    const std::string wav = std::string(SUJI_DEFAULT_MODELS_DIR)
        + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav";
    const std::string ffprobe = ffprobe_path();

    double dur = probe_duration_seconds(ffprobe, wav);
    CHECK(dur > 0.0);
    CHECK(dur < 60.0);
}

TEST_CASE("probe_duration_seconds returns -1.0 for nonexistent file") {
    double dur = probe_duration_seconds(ffprobe_path(), "nonexistent_file_xyz.wav");
    CHECK(dur == doctest::Approx(-1.0));
}
