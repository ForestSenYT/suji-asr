#include "doctest/doctest.h"
#include "core/paths.h"
#include <string>
#include <filesystem>

using namespace suji;

TEST_CASE("app_dir is non-empty") {
    CHECK(app_dir().size() > 0);
}

TEST_CASE("models_dir falls back to dev default when no app-relative models/ exists") {
    // suji_tests.exe lives in build/Release with NO models/ subdir next to it -> dev fallback
    CHECK(models_dir() == std::string(SUJI_DEFAULT_MODELS_DIR));
}

TEST_CASE("ffmpeg_path falls back to dev default when no app-relative ffmpeg.exe exists") {
    CHECK(ffmpeg_path() == std::string(SUJI_DEFAULT_FFMPEG));
}

TEST_CASE("cuda_dll_dir returns a dir containing the CUDA runtime, or empty") {
    std::string d = cuda_dll_dir();
    if (!d.empty()) {
        std::error_code ec;
        // when non-empty, the returned dir MUST actually contain the CUDA runtime marker
        CHECK(std::filesystem::exists(d + "/cudnn64_9.dll", ec));
    }
    // empty is valid (a machine with no CUDA runtime) — nothing to assert then
}

// T17: default_model_paths() — centralized model path construction
TEST_CASE("T17: default_model_paths returns non-empty strings consistent with models_dir") {
    auto mp = default_model_paths();
    const std::string mdl = models_dir();

    // All paths are non-empty
    CHECK(!mp.asr_model.empty());
    CHECK(!mp.tokens.empty());
    CHECK(!mp.vad_model.empty());
    CHECK(!mp.punct_model.empty());

    // All paths are rooted under models_dir()
    CHECK(mp.asr_model.find(mdl)   == 0);
    CHECK(mp.tokens.find(mdl)      == 0);
    CHECK(mp.vad_model.find(mdl)   == 0);
    CHECK(mp.punct_model.find(mdl) == 0);

    // Spot-check expected filenames
    CHECK(mp.asr_model.find("model.int8.onnx")  != std::string::npos);
    CHECK(mp.tokens.find("tokens.txt")           != std::string::npos);
    CHECK(mp.vad_model.find("silero_vad.onnx")  != std::string::npos);
    CHECK(mp.punct_model.find("model.int8.onnx") != std::string::npos);

    // All four paths are distinct
    CHECK(mp.asr_model   != mp.tokens);
    CHECK(mp.asr_model   != mp.vad_model);
    CHECK(mp.asr_model   != mp.punct_model);
    CHECK(mp.tokens      != mp.vad_model);
    CHECK(mp.tokens      != mp.punct_model);
    CHECK(mp.vad_model   != mp.punct_model);
}
