#include "core/paths.h"

#include <filesystem>
#include <string>

#ifdef _WIN32
#  define NOMINMAX
#  include <windows.h>
#endif

namespace suji {

namespace {
// Case-insensitive ASCII substring test. Used to match Qwen3 model filenames,
// whose exact casing/spelling varies by release (encoder/Encoder, etc.).
bool icontains_ascii(const std::string& hay, const std::string& needle) {
    auto lower = [](unsigned char ch) {
        return static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch);
    };
    std::string h(hay.size(), '\0'), n(needle.size(), '\0');
    for (size_t i = 0; i < hay.size(); ++i)    h[i] = lower(static_cast<unsigned char>(hay[i]));
    for (size_t i = 0; i < needle.size(); ++i) n[i] = lower(static_cast<unsigned char>(needle[i]));
    return h.find(n) != std::string::npos;
}
} // namespace

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

ModelPaths default_model_paths() {
    std::string mdl = models_dir();
    std::string m   = mdl + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
    ModelPaths p;
    p.asr_model   = m   + "model.int8.onnx";
    p.tokens      = m   + "tokens.txt";
    p.vad_model   = mdl + "/silero_vad.onnx";
    p.punct_model = mdl + "/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
    return p;
}

std::string discover_rule_fsts() {
    std::string mdl = models_dir();
    std::error_code ec;
    // Priority-ordered well-known names.
    for (const char* name : {"itn.fst", "itn.far"}) {
        auto candidate = mdl + "/" + name;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    // Fallback: first *.fst or *.far found directly in models_dir().
    for (const char* ext : {".fst", ".far"}) {
        for (auto& entry : std::filesystem::directory_iterator(mdl, ec)) {
            if (ec) break;
            if (entry.is_regular_file() && entry.path().extension() == ext)
                return entry.path().string();
        }
        if (ec) ec.clear();
    }
    return "";
}

AedModel discover_fp16_aed() {
    AedModel m;
    std::error_code ec;
    const std::string mdl = models_dir();

    for (auto& entry : std::filesystem::directory_iterator(mdl, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        // Match a dir named "sherpa-onnx-fire-red-asr-large-*fp16*".
        const std::string name = entry.path().filename().string();
        const std::string prefix = "sherpa-onnx-fire-red-asr-large-";
        if (name.rfind(prefix, 0) != 0) continue;     // must start with prefix
        if (name.find("fp16") == std::string::npos) continue;

        // Require all three files to be present before committing.
        auto enc = entry.path() / "encoder.fp16.onnx";
        auto dec = entry.path() / "decoder.fp16.onnx";
        auto tok = entry.path() / "tokens.txt";
        if (std::filesystem::exists(enc, ec) &&
            std::filesystem::exists(dec, ec) &&
            std::filesystem::exists(tok, ec)) {
            m.encoder = enc.string();
            m.decoder = dec.string();
            m.tokens  = tok.string();
            return m;   // first complete match wins
        }
    }
    return m;   // empty (ok()==false) when no complete fp16-AED model found
}

Qwen3Model discover_qwen3() {
    Qwen3Model m;
    std::error_code ec;
    const std::string mdl = models_dir();

    for (auto& entry : std::filesystem::directory_iterator(mdl, ec)) {
        if (ec) break;
        if (!entry.is_directory(ec)) continue;

        // Match a dir named "sherpa-onnx-qwen3-asr*".
        const std::string name = entry.path().filename().string();
        const std::string prefix = "sherpa-onnx-qwen3-asr";
        if (name.rfind(prefix, 0) != 0) continue;     // must start with prefix

        const std::filesystem::path mdir = entry.path();

        // Match the model .onnx files by substring (filenames vary by release):
        //   *conv*front* -> conv_frontend, *encoder* -> encoder, *decoder* -> decoder.
        std::string conv, enc, dec;
        for (auto& f : std::filesystem::directory_iterator(mdir, ec)) {
            if (ec) break;
            if (!f.is_regular_file(ec)) continue;
            if (f.path().extension() != ".onnx") continue;
            const std::string fn = f.path().filename().string();
            if (icontains_ascii(fn, "conv") && icontains_ascii(fn, "front")) conv = f.path().string();
            else if (icontains_ascii(fn, "encoder"))                         enc = f.path().string();
            else if (icontains_ascii(fn, "decoder"))                         dec = f.path().string();
        }
        if (ec) ec.clear();

        // Tokenizer DIRECTORY = the dir containing vocab.json (model dir or a subdir).
        std::string tok;
        if (std::filesystem::exists(mdir / "vocab.json", ec)) {
            tok = mdir.string();
        } else {
            for (auto& f : std::filesystem::recursive_directory_iterator(mdir, ec)) {
                if (ec) break;
                if (f.is_regular_file(ec) && f.path().filename() == "vocab.json") {
                    tok = f.path().parent_path().string();
                    break;
                }
            }
            if (ec) ec.clear();
        }

        if (!conv.empty() && !enc.empty() && !dec.empty() && !tok.empty()) {
            m.conv_frontend = conv;
            m.encoder       = enc;
            m.decoder       = dec;
            m.tokenizer     = tok;
            return m;   // first complete match wins
        }
    }
    return m;   // empty (ok()==false) when no complete Qwen3 model found
}

ModePlan plan_mode(TranscribeMode mode, bool qwen3_available,
                   bool aed_available, bool gpu_usable) {
    ModePlan plan;

    // Qwen3: most accurate, CPU-recommended. Falls back to AED if the model is absent.
    if (mode == TranscribeMode::Qwen3) {
        if (qwen3_available) {
            plan.model = ModeModel::Qwen3;
            plan.recommended_provider = Provider::Cpu;
            plan.provider_is_recommendation = true;
            return plan;
        }
        plan.fell_back = true;
        mode = TranscribeMode::Aed;   // fall through into the AED branch
    }

    // AED: fp16, GPU-only. Use it only when a CUDA GPU is usable AND the model exists;
    // otherwise leave the CTC default with no provider recommendation (graceful).
    if (mode == TranscribeMode::Aed) {
        if (gpu_usable && aed_available) {
            plan.model = ModeModel::Aed;
            plan.recommended_provider = Provider::Cuda;
            plan.provider_is_recommendation = true;
            return plan;
        }
        // No fp16/GPU available: fall back to the shipped CTC default, decide() picks.
        plan.model = ModeModel::Ctc;
        plan.provider_is_recommendation = false;
        return plan;
    }

    // CTC: default int8 model, no provider recommendation (decide() picks).
    plan.model = ModeModel::Ctc;
    plan.provider_is_recommendation = false;
    return plan;
}

} // namespace suji
