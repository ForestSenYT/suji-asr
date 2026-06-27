#include "doctest/doctest.h"
#include "core/config.h"

using namespace suji;

// ---- T3: decoding_method ----

TEST_CASE("T3: EngineConfig decoding_method default is greedy_search") {
  EngineConfig c;
  CHECK(c.decoding_method == "greedy_search");
}

TEST_CASE("T3: EngineConfig decoding_method can be overridden") {
  EngineConfig c;
  c.decoding_method = "beam_search";
  CHECK(c.decoding_method == "beam_search");
}

// ---- G10: punct provider / threads ----

TEST_CASE("G10: EngineConfig punct_provider default is cpu") {
  EngineConfig c;
  CHECK(c.punct_provider == "cpu");
}

TEST_CASE("G10: EngineConfig punct_threads default is 1") {
  EngineConfig c;
  CHECK(c.punct_threads == 1);
}

TEST_CASE("G10: EngineConfig punct_provider can be overridden") {
  EngineConfig c;
  c.punct_provider = "cuda";
  CHECK(c.punct_provider == "cuda");
}

TEST_CASE("G10: EngineConfig punct_threads can be overridden") {
  EngineConfig c;
  c.punct_threads = 4;
  CHECK(c.punct_threads == 4);
}

// ---- T4: rule_fars field ----

TEST_CASE("T4: EngineConfig rule_fars defaults to empty string") {
  EngineConfig c;
  CHECK(c.rule_fars.empty());
}

TEST_CASE("T4: EngineConfig rule_fars can be set") {
  EngineConfig c;
  c.rule_fars = "itn.far";
  CHECK(c.rule_fars == "itn.far");
}

// ---- P1: FireRedASR AED (encoder/decoder) selection ----

// Mirror of the branch predicate in asr.cpp: AED is used iff BOTH paths are set.
// Tested directly so we can verify selection logic without loading the ~2GB model.
static bool selects_aed(const EngineConfig& c) {
  return !c.asr_encoder.empty() && !c.asr_decoder.empty();
}

TEST_CASE("P1: EngineConfig asr_encoder/asr_decoder default to empty (CTC path)") {
  EngineConfig c;
  CHECK(c.asr_encoder.empty());
  CHECK(c.asr_decoder.empty());
  CHECK_FALSE(selects_aed(c));   // default = legacy CTC path, unchanged
}

TEST_CASE("P1: both encoder and decoder set selects the AED branch") {
  EngineConfig c;
  c.asr_encoder = "encoder.fp16.onnx";
  c.asr_decoder = "decoder.fp16.onnx";
  CHECK(selects_aed(c));
}

TEST_CASE("P1: only encoder set falls back to CTC (not AED)") {
  EngineConfig c;
  c.asr_encoder = "encoder.fp16.onnx";
  CHECK_FALSE(selects_aed(c));
}

TEST_CASE("P1: only decoder set falls back to CTC (not AED)") {
  EngineConfig c;
  c.asr_decoder = "decoder.fp16.onnx";
  CHECK_FALSE(selects_aed(c));
}

TEST_CASE("P1: setting AED paths does not disturb asr_model (CTC) field") {
  EngineConfig c;
  c.asr_model   = "ctc.int8.onnx";
  c.asr_encoder = "encoder.fp16.onnx";
  c.asr_decoder = "decoder.fp16.onnx";
  CHECK(c.asr_model == "ctc.int8.onnx");   // CTC field preserved, just not used
  CHECK(selects_aed(c));
}

// ---- Qwen3 hotwords: EngineConfig field ----

TEST_CASE("hotwords: EngineConfig.hotwords defaults to empty (no biasing)") {
  EngineConfig c;
  CHECK(c.hotwords.empty());
}

TEST_CASE("hotwords: EngineConfig.hotwords can be set to a file path") {
  EngineConfig c;
  c.hotwords = "models/lecture_hotwords_zh.txt";
  CHECK(c.hotwords == "models/lecture_hotwords_zh.txt");
}
