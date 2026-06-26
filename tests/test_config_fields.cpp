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
