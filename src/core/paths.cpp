#include "core/paths.h"

#include <filesystem>
#include <string>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

namespace suji {

std::string app_dir() {
#ifdef _WIN32
    // Start with MAX_PATH; resize and retry if the buffer was too small.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return ".";
        if (len < static_cast<DWORD>(buf.size())) {
            buf.resize(len);
            break;
        }
        // Buffer too small — double it and retry.
        buf.resize(buf.size() * 2, L'\0');
    }

    // Convert wide path to UTF-8.
    int needed = WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return ".";
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()),
                        utf8.data(), needed, nullptr, nullptr);

    auto parent = std::filesystem::path(utf8).parent_path();
    return parent.string();
#else
    return ".";
#endif
}

std::string models_dir() {
    auto d = app_dir() + "/models";
    std::error_code ec;
    if (std::filesystem::exists(d, ec)) return d;
    return SUJI_DEFAULT_MODELS_DIR;
}

std::string ffmpeg_path() {
    auto f = app_dir() + "/ffmpeg.exe";
    std::error_code ec;
    if (std::filesystem::exists(f, ec)) return f;
    return SUJI_DEFAULT_FFMPEG;
}

std::string ffprobe_path() {
    auto f = app_dir() + "/ffprobe.exe";
    std::error_code ec;
    if (std::filesystem::exists(f, ec)) return f;
    // Dev fallback: same directory as SUJI_DEFAULT_FFMPEG, filename ffprobe.exe
    return std::filesystem::path(SUJI_DEFAULT_FFMPEG).replace_filename("ffprobe.exe").string();
}

std::string cuda_dll_dir() {
    std::error_code ec;
    // 1. Check next to the running exe first (production install layout).
    auto app_candidate = app_dir() + "/cudnn64_9.dll";
    if (std::filesystem::exists(app_candidate, ec)) return app_dir();
    // 2. Fall back to the dev-tree vendor location.
    auto dev_candidate = std::string(SUJI_DEFAULT_CUDA_DLL_DIR) + "/cudnn64_9.dll";
    if (std::filesystem::exists(dev_candidate, ec)) return std::string(SUJI_DEFAULT_CUDA_DLL_DIR);
    // 3. CUDA runtime DLLs not found.
    return "";
}

} // namespace suji
