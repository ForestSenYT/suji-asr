#include "doctest/doctest.h"
#include "core/media_decode.h"
#include "core/cancel.h"
#include "core/paths.h"
#include <string>
#include <filesystem>
#include <chrono>
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

// A pre-cancelled token must abort decode of a long file quickly (TerminateProcess),
// returning false with err == "cancelled" in well under 2 s.
TEST_CASE("decode_to_pcm respects pre-cancelled token" * doctest::timeout(30)) {
    // Build a ~120-second WAV by looping the short test wav inside ffmpeg.
    wchar_t tmp_buf[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp_buf);
    std::wstring tmp_dir_w(tmp_buf);
    if (!tmp_dir_w.empty() && tmp_dir_w.back() == L'\\') tmp_dir_w.pop_back();
    std::wstring long_w = tmp_dir_w + L"\\suji_cancel_test_long.wav";

    // UTF-8 version of the long-wav path for the std::string decode API.
    int n = WideCharToMultiByte(CP_UTF8, 0, long_w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string long_wav(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, long_w.c_str(), -1, long_wav.data(), n, nullptr, nullptr);

    // Create the long file once via ffmpeg -stream_loop (output discarded to NUL).
    {
        std::wstring src_w    = to_wide(wav0());
        std::wstring ffmpeg_w = to_wide(ffmpeg_exe());
        std::wstring cmd = L"\"" + ffmpeg_w + L"\" -y -stream_loop 15 -i \""
                         + src_w + L"\" -t 120 -ar 16000 -ac 1 \"" + long_w + L"\"";

        SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{}; si.cb = sizeof(si);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = nullptr; si.hStdOutput = nul; si.hStdError = nul;
        PROCESS_INFORMATION pi{};
        BOOL spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        REQUIRE_MESSAGE(spawned, "ffmpeg -stream_loop failed to spawn");
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        REQUIRE(fs::exists(long_wav));
    }

    // Pre-cancel, then decode: must abort fast and report "cancelled".
    suji::CancelToken tok;
    tok.cancel();

    suji::AudioBuffer ab; std::string err;
    auto t0 = std::chrono::steady_clock::now();
    bool result = suji::decode_to_pcm(ffmpeg_exe(), long_wav, ab, err, &tok);
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    CHECK_FALSE(result);
    CHECK(err == "cancelled");
    CHECK(elapsed < 2.0);

    fs::remove(long_wav);

    // Regression: NULL-cancel decode of the short wav still succeeds.
    suji::AudioBuffer ab2; std::string err2;
    bool ok2 = suji::decode_to_pcm(ffmpeg_exe(), wav0(), ab2, err2);
    INFO("regression decode err: " << err2);
    CHECK(ok2);
    CHECK(ab2.samples.size() > 0);
}
