#include "doctest/doctest.h"
#include "core/paths.h"
#include <string>

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

TEST_CASE("cuda_dll_dir falls back to dev default when cudnn64_9.dll is in vendor tree") {
    // suji_tests.exe lives in build/Release with NO cudnn64_9.dll next to it,
    // so cuda_dll_dir() falls through to SUJI_DEFAULT_CUDA_DLL_DIR (vendor/cuda-redist/dll).
    CHECK(cuda_dll_dir() == std::string(SUJI_DEFAULT_CUDA_DLL_DIR));
}
