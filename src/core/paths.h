#pragma once
#include <string>
#include "core/config.h"   // Provider

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

/// Auto-discover an ITN FST asset in models_dir().
/// Looks for "itn.fst", "itn.far", or any *.fst / *.far directly under models_dir().
/// Returns the path of the first match, or "" if none found (ITN stays OFF by default).
std::string discover_rule_fsts();

/// Result of discover_fp16_aed(): absolute paths to the fp16 FireRedASR AED model
/// (encoder + decoder + tokens). ok() is true only when all three are present.
struct AedModel {
    std::string encoder, decoder, tokens;
    bool ok() const { return !encoder.empty() && !decoder.empty() && !tokens.empty(); }
};

/// Auto-discover the fp16 FireRedASR AED model under models_dir().
/// Scans for a directory matching "sherpa-onnx-fire-red-asr-large-*fp16*" that
/// contains encoder.fp16.onnx + decoder.fp16.onnx + tokens.txt; returns their
/// absolute paths if ALL three are present, else an empty AedModel (ok()==false).
/// UTF-8-safe (std::filesystem with error_code; no throw). NOTE: fp16 is GPU-only
/// (it crashes on the CPU EP) — callers must gate use on a working CUDA GPU.
AedModel discover_fp16_aed();

/// Result of discover_qwen3(): absolute paths to the Qwen3-ASR model files
/// (conv-frontend + encoder + decoder ONNX) plus the tokenizer DIRECTORY (the dir
/// containing vocab.json). ok() is true only when all four are present.
struct Qwen3Model {
    std::string conv_frontend, encoder, decoder, tokenizer;
    bool ok() const {
        return !conv_frontend.empty() && !encoder.empty()
            && !decoder.empty() && !tokenizer.empty();
    }
};

/// Auto-discover the Qwen3-ASR model under models_dir().
/// Scans for a directory matching "sherpa-onnx-qwen3-asr*" and, inside it, matches
/// the .onnx files by substring (*conv*front* -> conv_frontend, *encoder* -> encoder,
/// *decoder* -> decoder) and locates the tokenizer dir (the dir containing vocab.json,
/// the model dir itself or a subdir). Returns their absolute paths if ALL four are
/// present, else an empty Qwen3Model (ok()==false). Mirrors discover_fp16_aed():
/// UTF-8-safe (std::filesystem with error_code; no throw).
Qwen3Model discover_qwen3();

// ---------------------------------------------------------------------------
// Transcription-mode model/provider planner (pure; no hardware probe, no I/O).
// Captures the GUI's mode -> model + recommended-provider decision table so it is
// testable in isolation. The GUI's EngineWorker::run() calls this AFTER probing
// for the models + GPU, then applies the result to its EngineConfig/AutoTune.
// ---------------------------------------------------------------------------

/// Which ASR model the chosen mode resolved to.
enum class ModeModel { Qwen3, Aed, Ctc };

/// Transcription mode (mirror of gui Mode enum; kept Qt-free in core).
/// 0=Qwen3 (准确度, default), 1=AED (速度), 2=CTC (词级字幕).
enum class TranscribeMode { Qwen3 = 0, Aed = 1, Ctc = 2 };

/// Result of plan_mode(): the model to use and the recommended provider.
/// `provider_is_recommendation` is true when the provider came from the mode's
/// default (caller applies it only if the user left provider on "auto"); false
/// means the mode imposes no provider preference (CTC -> let decide() pick).
struct ModePlan {
    ModeModel model = ModeModel::Ctc;
    Provider  recommended_provider = Provider::Cpu;
    bool      provider_is_recommendation = false;
    bool      fell_back = false;   ///< Qwen3 requested but not found -> AED chosen
};

/// Pure mode -> (model, recommended provider) mapping.
///   mode Qwen3: if qwen3_available -> Qwen3 model, recommend CPU. Else fall back
///     to the AED branch (fell_back=true).
///   mode AED: if a CUDA GPU is usable (gpu_usable) AND aed_available -> AED model,
///     recommend CUDA. Else no model override (Ctc default) and no recommendation
///     (provider stays decide()'s pick) — graceful when fp16/GPU is absent.
///   mode CTC: Ctc model, no provider recommendation (decide() picks).
/// gpu_usable should already fold in "user didn't force cpu/hetero" + a working
/// CUDA runtime at the call site; this function only decides the table.
ModePlan plan_mode(TranscribeMode mode, bool qwen3_available,
                   bool aed_available, bool gpu_usable);

} // namespace suji
