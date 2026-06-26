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
