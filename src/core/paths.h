#pragma once
#include <string>

namespace suji {

/// Directory of the running executable (no trailing slash), or "." on failure.
std::string app_dir();

/// app_dir()+"/models" if it exists, else SUJI_DEFAULT_MODELS_DIR (dev fallback).
std::string models_dir();

/// app_dir()+"/ffmpeg.exe" if it exists, else SUJI_DEFAULT_FFMPEG (dev fallback).
std::string ffmpeg_path();

/// app_dir()+"/ffprobe.exe" if it exists, else same directory as SUJI_DEFAULT_FFMPEG with filename ffprobe.exe.
std::string ffprobe_path();

/// Dir containing the CUDA runtime DLLs, or "" if none found.
/// app_dir() if cudnn64_9.dll is there; else SUJI_DEFAULT_CUDA_DLL_DIR if cudnn64_9.dll is there; else "".
std::string cuda_dll_dir();

/// Centralized model paths built from models_dir() — single source of truth for all binaries.
/// Replaces the inline "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/..." copy-paste
/// that previously appeared in src/cli/main.cpp, src/cli/batch_main.cpp, and
/// src/gui/engine_worker.cpp.
struct ModelPaths {
    std::string asr_model;   ///< ASR ONNX (FireRedASR2-CTC int8)
    std::string tokens;      ///< tokens.txt for the ASR model
    std::string vad_model;   ///< Silero VAD ONNX
    std::string punct_model; ///< CT punctuation transformer ONNX
};

/// Build ModelPaths from models_dir(). All paths are absolute (or relative to CWD on failure).
/// Returns non-empty strings; the files may not exist on a dev machine without models downloaded.
ModelPaths default_model_paths();

} // namespace suji
