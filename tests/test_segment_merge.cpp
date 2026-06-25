#include "doctest/doctest.h"
#include "core/segment_merge.h"

using namespace suji;

static Token T(const char* s, double t) {
  Token x;
  x.text = s;
  x.start = t;
  return x;
}

TEST_CASE("merge by gap") {
  std::vector<Token> toks = { T("你",0.0),T("好",0.3),T("世",2.0),T("界",2.2) }; // 0.3->2.0 gap=1.7
  auto segs = merge_tokens(toks, 1.0, 30.0);
  REQUIRE(segs.size() == 2);
  CHECK(segs[0].text == u8"你好");
  CHECK(segs[0].start == doctest::Approx(0.0));
  CHECK(segs[0].end   == doctest::Approx(0.3));
  CHECK(segs[1].text == u8"世界");
}

TEST_CASE("merge by max duration") {
  std::vector<Token> toks;
  for(int i=0;i<10;i++) toks.push_back(T("字", i*0.5)); // 连续 0.5s 间隔
  auto segs = merge_tokens(toks, 1.0, 2.0); // 2s 上限 -> 多段
  CHECK(segs.size() >= 2);
}

TEST_CASE("empty input") {
  CHECK(merge_tokens({},1.0,30.0).empty());
}
