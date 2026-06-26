#include "doctest/doctest.h"
#include "core/media_decode.h"
#include "core/paths.h"
#include <string>
#include <filesystem>
#include <windows.h>

namespace fs = std::filesystem;

static std::string ffmpeg_exe() { return suji::ffmpeg_path(); }
static std::string wav0() {
    return std::string(SUJI_DEFAULT_MODELS_DIR) +
           "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav";
}

// Helper: UTF-8 string -> wstring (for Win32 calls)
static std::wstring to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
    return w;
}

TEST_CASE("decode wav to 16k mono pcm" * doctest::timeout(60)) {
    suji::AudioBuffer ab; std::string err;
    REQUIRE_MESSAGE(suji::decode_to_pcm(ffmpeg_exe(), wav0(), ab, err), err);
    CHECK(ab.sample_rate == 16000);
    CHECK(ab.samples.size() > 120000);   // >7.5s
    CHECK(ab.samples.size() < 200000);   // <12.5s
}

TEST_CASE("decode missing file fails") {
    suji::AudioBuffer ab; std::string err;
    CHECK_FALSE(suji::decode_to_pcm(ffmpeg_exe(), "no_such_file.wav", ab, err));
}

// RED TEST: Chinese filename with space and % should decode successfully.
// With the old _popen / cmd.exe code this FAILS because UTF-8 is mangled
// through the ANSI codepage and % is a cmd.exe metacharacter.
TEST_CASE("decode wav with Chinese and percent-sign filename" * doctest::timeout(60)) {
    // Build a temp path with Chinese characters + space + % (cmd.exe metachar)
    // Use u8 literal so MSVC /utf-8 embeds it as UTF-8 bytes.
    const std::string chinese_filename = u8"测试 100%讲座.wav";

    // Get system temp dir as UTF-8
    wchar_t tmp_buf[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp_buf);
    std::wstring tmp_dir_w(tmp_buf);
    // Remove trailing backslash
    if (!tmp_dir_w.empty() && tmp_dir_w.back() == L'\\')
        tmp_dir_w.pop_back();

    std::wstring dest_w = tmp_dir_w + L"\\" + to_wide(chinese_filename);

    // Convert source (ASCII) path to wstring for CopyFileW
    std::wstring src_w = to_wide(wav0());

    // CopyFileW handles UTF-16 paths correctly
    BOOL copied = CopyFileW(src_w.c_str(), dest_w.c_str(), FALSE);
    REQUIRE_MESSAGE(copied, "CopyFileW failed: could not create Chinese-named temp file");

    // Build UTF-8 version of the dest path for decode_to_pcm
    int n = WideCharToMultiByte(CP_UTF8, 0, dest_w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string dest_utf8(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, dest_w.c_str(), -1, dest_utf8.data(), n, nullptr, nullptr);

    suji::AudioBuffer ab; std::string err;
    bool ok = suji::decode_to_pcm(ffmpeg_exe(), dest_utf8, ab, err);

    // Cleanup regardless of result
    DeleteFileW(dest_w.c_str());

    INFO("decode_to_pcm failed for Chinese/percent filename: " << err);
    REQUIRE(ok);
    CHECK(!ab.samples.empty());
    CHECK(ab.sample_rate == 16000);
}
