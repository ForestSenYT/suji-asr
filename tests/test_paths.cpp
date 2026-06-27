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

// T4: discover_rule_fsts() — returns empty string when no FST/FAR asset present
TEST_CASE("T4: discover_rule_fsts returns empty when no ITN asset present") {
    // In the dev/test environment (build/Release), models_dir() falls back to
    // SUJI_DEFAULT_MODELS_DIR which exists but has no itn.fst / itn.far.
    // This verifies: (a) doesn't crash, (b) returns "" (ITN off by default).
    std::string result = discover_rule_fsts();
    // Either empty (no asset shipped) or a valid existing file path.
    if (!result.empty()) {
        std::error_code ec;
        CHECK(std::filesystem::exists(result, ec));
    }
    // In the current repo state no ITN asset is shipped -> should be empty.
    CHECK(result.empty());
}

// T17: default_model_paths() — centralized model path construction
TEST_CASE("T17: default_model_paths returns non-empty strings consistent with models_dir") {
    auto mp = default_model_paths();
    const std::string mdl = models_dir();

    // All paths are non-empty
    CHECK(!mp.asr_model.empty());
    CHECK(!mp.tokens.empty());
    CHECK(!mp.vad_model.empty());
    CHECK(!mp.punct_model.empty());

    // All paths are rooted under models_dir()
    CHECK(mp.asr_model.find(mdl)   == 0);
    CHECK(mp.tokens.find(mdl)      == 0);
    CHECK(mp.vad_model.find(mdl)   == 0);
    CHECK(mp.punct_model.find(mdl) == 0);

    // Spot-check expected filenames
    CHECK(mp.asr_model.find("model.int8.onnx")  != std::string::npos);
    CHECK(mp.tokens.find("tokens.txt")           != std::string::npos);
    CHECK(mp.vad_model.find("silero_vad.onnx")  != std::string::npos);
    CHECK(mp.punct_model.find("model.int8.onnx") != std::string::npos);

    // All four paths are distinct
    CHECK(mp.asr_model   != mp.tokens);
    CHECK(mp.asr_model   != mp.vad_model);
    CHECK(mp.asr_model   != mp.punct_model);
    CHECK(mp.tokens      != mp.vad_model);
    CHECK(mp.tokens      != mp.punct_model);
    CHECK(mp.vad_model   != mp.punct_model);
}

// discover_fp16_aed() — auto-detect the fp16 FireRedASR AED model in models_dir().
TEST_CASE("discover_fp16_aed: returns empty or a complete, existing AedModel") {
    AedModel m = discover_fp16_aed();
    std::error_code ec;
    if (m.ok()) {
        // When ok(), ALL three paths must exist and be the fp16 AED filenames.
        CHECK(std::filesystem::exists(m.encoder, ec));
        CHECK(std::filesystem::exists(m.decoder, ec));
        CHECK(std::filesystem::exists(m.tokens, ec));
        CHECK(m.encoder.find("encoder.fp16.onnx") != std::string::npos);
        CHECK(m.decoder.find("decoder.fp16.onnx") != std::string::npos);
        CHECK(m.tokens.find("tokens.txt")          != std::string::npos);
        // The dir matches the expected fp16-AED naming pattern.
        CHECK(m.encoder.find("sherpa-onnx-fire-red-asr-large-") != std::string::npos);
        CHECK(m.encoder.find("fp16") != std::string::npos);
    } else {
        // ok()==false means an incomplete/empty result: all three must be empty.
        CHECK(m.encoder.empty());
        CHECK(m.decoder.empty());
        CHECK(m.tokens.empty());
    }
}

// ok() semantics: requires all three non-empty.
TEST_CASE("AedModel::ok() is true only when all three paths are set") {
    AedModel a;
    CHECK_FALSE(a.ok());
    a.encoder = "e"; CHECK_FALSE(a.ok());
    a.decoder = "d"; CHECK_FALSE(a.ok());
    a.tokens  = "t"; CHECK(a.ok());
}

// discover_qwen3() — auto-detect the Qwen3-ASR model in models_dir().
TEST_CASE("discover_qwen3: returns empty or a complete, existing Qwen3Model") {
    Qwen3Model m = discover_qwen3();
    std::error_code ec;
    if (m.ok()) {
        // When ok(), all four paths must exist (3 files + tokenizer DIR).
        CHECK(std::filesystem::exists(m.conv_frontend, ec));
        CHECK(std::filesystem::exists(m.encoder, ec));
        CHECK(std::filesystem::exists(m.decoder, ec));
        CHECK(std::filesystem::exists(m.tokenizer, ec));
        // The tokenizer is a DIRECTORY containing vocab.json.
        CHECK(std::filesystem::is_directory(m.tokenizer, ec));
        CHECK(std::filesystem::exists(m.tokenizer + "/vocab.json", ec));
        // Discovered dir matches the expected Qwen3 naming pattern.
        CHECK(m.encoder.find("sherpa-onnx-qwen3-asr") != std::string::npos);
        CHECK(m.encoder.find("encoder") != std::string::npos);
        CHECK(m.decoder.find("decoder") != std::string::npos);
    } else {
        // ok()==false: an incomplete/empty result -> all four empty.
        CHECK(m.conv_frontend.empty());
        CHECK(m.encoder.empty());
        CHECK(m.decoder.empty());
        CHECK(m.tokenizer.empty());
    }
}

// Qwen3Model::ok() requires all four paths non-empty.
TEST_CASE("Qwen3Model::ok() is true only when all four paths are set") {
    Qwen3Model q;
    CHECK_FALSE(q.ok());
    q.conv_frontend = "c"; CHECK_FALSE(q.ok());
    q.encoder       = "e"; CHECK_FALSE(q.ok());
    q.decoder       = "d"; CHECK_FALSE(q.ok());
    q.tokenizer     = "t"; CHECK(q.ok());
}

// plan_mode() — the mode -> (model, recommended provider) decision table.
TEST_CASE("plan_mode: Qwen3 mode picks Qwen3 + recommends CPU when model present") {
    ModePlan p = plan_mode(TranscribeMode::Qwen3, /*qwen3*/true, /*aed*/true, /*gpu*/true);
    CHECK(p.model == ModeModel::Qwen3);
    CHECK(p.recommended_provider == Provider::Cpu);
    CHECK(p.provider_is_recommendation);
    CHECK_FALSE(p.fell_back);
}

TEST_CASE("plan_mode: Qwen3 mode falls back to AED on GPU when Qwen3 absent") {
    ModePlan p = plan_mode(TranscribeMode::Qwen3, /*qwen3*/false, /*aed*/true, /*gpu*/true);
    CHECK(p.model == ModeModel::Aed);
    CHECK(p.recommended_provider == Provider::Cuda);
    CHECK(p.provider_is_recommendation);
    CHECK(p.fell_back);
}

TEST_CASE("plan_mode: Qwen3 mode falls back to CTC when neither Qwen3 nor a GPU AED is usable") {
    // Qwen3 missing, no usable GPU -> AED branch can't run -> CTC default, no rec.
    ModePlan p = plan_mode(TranscribeMode::Qwen3, /*qwen3*/false, /*aed*/true, /*gpu*/false);
    CHECK(p.model == ModeModel::Ctc);
    CHECK_FALSE(p.provider_is_recommendation);
    CHECK(p.fell_back);
}

TEST_CASE("plan_mode: AED mode picks AED + recommends CUDA when GPU + model present") {
    ModePlan p = plan_mode(TranscribeMode::Aed, /*qwen3*/false, /*aed*/true, /*gpu*/true);
    CHECK(p.model == ModeModel::Aed);
    CHECK(p.recommended_provider == Provider::Cuda);
    CHECK(p.provider_is_recommendation);
}

TEST_CASE("plan_mode: AED mode without a usable GPU falls back to CTC (no provider rec)") {
    ModePlan p = plan_mode(TranscribeMode::Aed, /*qwen3*/false, /*aed*/true, /*gpu*/false);
    CHECK(p.model == ModeModel::Ctc);
    CHECK_FALSE(p.provider_is_recommendation);
}

TEST_CASE("plan_mode: AED mode without the fp16 model falls back to CTC") {
    ModePlan p = plan_mode(TranscribeMode::Aed, /*qwen3*/false, /*aed*/false, /*gpu*/true);
    CHECK(p.model == ModeModel::Ctc);
    CHECK_FALSE(p.provider_is_recommendation);
}

TEST_CASE("plan_mode: CTC mode always picks CTC with no provider recommendation") {
    ModePlan p = plan_mode(TranscribeMode::Ctc, /*qwen3*/true, /*aed*/true, /*gpu*/true);
    CHECK(p.model == ModeModel::Ctc);
    CHECK_FALSE(p.provider_is_recommendation);
    CHECK_FALSE(p.fell_back);
}
