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
  // T6: end = start of next segment's first token (2.0), not last token in seg (0.3)
  CHECK(segs[0].end   == doctest::Approx(2.0));
  CHECK(segs[1].text == u8"世界");
  // T6: last seg: last_token.start + 0.3 = 2.2 + 0.3 = 2.5
  CHECK(segs[1].end   == doctest::Approx(2.5));
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

// ---- T6: end-time property tests ----

TEST_CASE("T6: each segment end > start") {
  std::vector<Token> toks = { T("你",0.0),T("好",0.3),T("世",2.0),T("界",2.2) };
  auto segs = merge_tokens(toks, 1.0, 30.0);
  for (const auto& s : segs) {
    CHECK(s.end > s.start);
  }
}

TEST_CASE("T6: segment end <= next segment start (monotonic)") {
  std::vector<Token> toks = { T("a",0.0),T("b",0.5),T("c",2.0),T("d",2.5),T("e",5.0),T("f",5.5) };
  auto segs = merge_tokens(toks, 1.0, 30.0);
  REQUIRE(segs.size() >= 2);
  for (size_t i = 0; i + 1 < segs.size(); ++i) {
    // end of seg[i] == start of seg[i+1] (next token's start), so end <= start[i+1]
    CHECK(segs[i].end <= segs[i+1].start + 1e-9);
  }
}

TEST_CASE("T6: last segment end = last_token.start + 0.3") {
  std::vector<Token> toks = { T("a",0.0),T("b",0.5) };
  auto segs = merge_tokens(toks, 10.0, 30.0); // no split -> 1 segment
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].end == doctest::Approx(0.5 + 0.3));
}

TEST_CASE("T6: single token segment end = token.start + 0.3") {
  std::vector<Token> toks = { T("a",1.5) };
  auto segs = merge_tokens(toks, 1.0, 30.0);
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].end == doctest::Approx(1.5 + 0.3));
}
