#pragma once
#include <string>

namespace suji {

/// Directory of the running executable (no trailing slash), or "." on failure.
std::string app_dir();

/// app_dir()+"/models" if it exists, else SUJI_DEFAULT_MODELS_DIR (dev fallback).
std::string models_dir();

/// app_dir()+"/ffmpeg.exe" if it exists, else SUJI_DEFAULT_FFMPEG (dev fallback).
std::string ffmpeg_path();

/// Dir containing the CUDA runtime DLLs, or "" if none found.
/// app_dir() if cudnn64_9.dll is there; else SUJI_DEFAULT_CUDA_DLL_DIR if cudnn64_9.dll is there; else "".
std::string cuda_dll_dir();

} // namespace suji
