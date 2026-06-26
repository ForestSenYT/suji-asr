#define NOMINMAX
#include <windows.h>
#include "core/media_decode.h"
#include <vector>
#include <string>

namespace suji {

// Convert a UTF-8 std::string to a UTF-16 std::wstring.
static std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
    return w;
}

bool decode_to_pcm(const std::string& ffmpeg, const std::string& input,
                   AudioBuffer& out, std::string& err) {
    // --- Convert paths from UTF-8 to UTF-16 ---
    std::wstring ffmpeg_w = utf8_to_wide(ffmpeg);
    std::wstring input_w  = utf8_to_wide(input);

    if (ffmpeg_w.empty()) { err = "decode_to_pcm: empty ffmpeg path"; return false; }
    if (input_w.empty())  { err = "decode_to_pcm: empty input path";  return false; }

    // --- Build the command-line wstring (mutable buffer required by CreateProcessW) ---
    // Format: "<ffmpeg>" -nostdin -loglevel error -i "<input>" -vn -ar 16000 -ac 1 -f f32le -
    // Windows filenames cannot contain '"', so no internal-quote escaping is needed.
    // No shell involvement -> %, &, ^, | are all literal.
    std::wstring cmdline = L"\"" + ffmpeg_w + L"\" -nostdin -loglevel error -i \""
                         + input_w + L"\" -vn -ar 16000 -ac 1 -f f32le -";

    // --- Create anonymous pipe for stdout (PCM data) ---
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;  // child inherits the write end

    HANDLE pipe_read  = INVALID_HANDLE_VALUE;
    HANDLE pipe_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) {
        err = "decode_to_pcm: CreatePipe failed (err=" + std::to_string(GetLastError()) + ")";
        return false;
    }

    // Make the READ end non-inheritable so only the child gets the write end.
    if (!SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0)) {
        err = "decode_to_pcm: SetHandleInformation failed (err=" + std::to_string(GetLastError()) + ")";
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return false;
    }

    // --- Open NUL device for stderr (prevents PCM stream corruption) ---
    HANDLE nul_handle = CreateFileW(L"NUL", GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa,             // inheritable
                                    OPEN_EXISTING, 0, nullptr);
    if (nul_handle == INVALID_HANDLE_VALUE) {
        err = "decode_to_pcm: failed to open NUL device (err=" + std::to_string(GetLastError()) + ")";
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return false;
    }

    // --- Configure child process startup ---
    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = nullptr;     // ffmpeg uses -nostdin
    si.hStdOutput = pipe_write;  // PCM stream
    si.hStdError  = nul_handle;  // discard ffmpeg diagnostic output

    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        nullptr,          // application name (use cmdline)
        cmdline.data(),   // mutable command line (CreateProcessW may modify it)
        nullptr,          // process security attributes
        nullptr,          // thread security attributes
        TRUE,             // inherit handles (pipe_write + nul_handle)
        CREATE_NO_WINDOW, // no console window
        nullptr,          // inherit environment
        nullptr,          // inherit current directory
        &si, &pi);

    // Parent no longer needs these ends — close before reading so EOF propagates.
    CloseHandle(pipe_write);
    CloseHandle(nul_handle);

    if (!ok) {
        err = "decode_to_pcm: CreateProcessW failed (err=" + std::to_string(GetLastError()) + ")";
        CloseHandle(pipe_read);
        return false;
    }

    // --- Read loop: accumulate raw PCM bytes from child's stdout ---
    out.samples.clear();
    out.sample_rate = 16000;

    std::vector<BYTE> raw_bytes;
    raw_bytes.reserve(4 * 65536);  // pre-alloc ~256 KB

    std::vector<BYTE> chunk(65536);
    DWORD bytes_read = 0;
    while (ReadFile(pipe_read, chunk.data(), static_cast<DWORD>(chunk.size()), &bytes_read, nullptr)
           && bytes_read > 0) {
        raw_bytes.insert(raw_bytes.end(), chunk.begin(), chunk.begin() + bytes_read);
    }

    CloseHandle(pipe_read);

    // Wait for ffmpeg to exit and retrieve its exit code.
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // --- Reinterpret accumulated bytes as float32 samples ---
    size_t n_floats = raw_bytes.size() / sizeof(float);
    if (n_floats == 0) {
        err = "ffmpeg produced no audio (exit=" + std::to_string(exit_code) + ")";
        return false;
    }

    out.samples.resize(n_floats);
    // memcpy is defined for type-punning in C++17 (std::byte / unsigned char aliasing rule)
    std::memcpy(out.samples.data(), raw_bytes.data(), n_floats * sizeof(float));

    return true;
}

double probe_duration_seconds(const std::string& ffprobe, const std::string& input) {
    std::wstring ffprobe_w = utf8_to_wide(ffprobe);
    std::wstring input_w   = utf8_to_wide(input);

    if (ffprobe_w.empty() || input_w.empty()) return -1.0;

    std::wstring cmdline = L"\"" + ffprobe_w
        + L"\" -v error -show_entries format=duration"
        + L" -of default=noprint_wrappers=1:nokey=1 \""
        + input_w + L"\"";

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE pipe_read  = INVALID_HANDLE_VALUE;
    HANDLE pipe_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) return -1.0;

    if (!SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(pipe_read); CloseHandle(pipe_write);
        return -1.0;
    }

    HANDLE nul_handle = CreateFileW(L"NUL", GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, 0, nullptr);
    if (nul_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_read); CloseHandle(pipe_write);
        return -1.0;
    }

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = nullptr;
    si.hStdOutput = pipe_write;
    si.hStdError  = nul_handle;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                             TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(pipe_write);
    CloseHandle(nul_handle);

    if (!ok) { CloseHandle(pipe_read); return -1.0; }

    // Read stdout text (the duration value)
    std::string text;
    std::vector<BYTE> chunk(256);
    DWORD bytes_read = 0;
    while (ReadFile(pipe_read, chunk.data(), static_cast<DWORD>(chunk.size()),
                    &bytes_read, nullptr) && bytes_read > 0) {
        text.append(reinterpret_cast<char*>(chunk.data()), bytes_read);
    }
    CloseHandle(pipe_read);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (text.empty()) return -1.0;

    try {
        size_t pos = 0;
        double d = std::stod(text, &pos);
        return (pos > 0) ? d : -1.0;
    } catch (...) {
        return -1.0;
    }
}

} // namespace suji
