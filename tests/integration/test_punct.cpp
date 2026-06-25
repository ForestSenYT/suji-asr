#include "doctest/doctest.h"
#include "core/punctuation.h"
#include "core/config.h"
using namespace suji;
TEST_CASE("punct adds marks" * doctest::timeout(60)) {
  EngineConfig c; c.punct_model=std::string(SUJI_DEFAULT_MODELS_DIR)+
    "/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  Punctuator p(c); REQUIRE(p.ok());
  std::string out = p.add(u8"今天天气怎么样明天呢");
  CHECK(out.size() >= std::string(u8"今天天气怎么样明天呢").size()); // 至少不更短(加了标点)
  CHECK(out != u8"今天天气怎么样明天呢");                              // 有变化
}
