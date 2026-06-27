#include "doctest/doctest.h"
#include "core/segment_merge.h"
#include <string>

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

// ---- merge_file_tokens: STABLE ordering (fp16-AED has no per-token timestamps) ----

TEST_CASE("merge_file_tokens: equal timestamps keep emission order (fp16-AED no-scramble)") {
  // fp16-AED emits no per-token timestamps -> every token in a segment shares one base
  // time. An UNSTABLE sort scrambles these equal-key tokens (the real bug); stable must
  // preserve the model's left-to-right emission order. 64 tokens makes introsort actually
  // reorder equal keys, so this would FAIL if merge_file_tokens reverted to std::sort.
  std::vector<Token> toks;
  std::string expected;
  for (int i = 0; i < 64; ++i) {
    std::string s = std::to_string(i);
    toks.push_back(T(s.c_str(), 1.5));   // ALL the same timestamp
    expected += s;
  }
  auto segs = merge_file_tokens(toks, 1.0, 1000.0);   // no gaps -> single segment
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].text == expected);                    // order preserved
}

TEST_CASE("merge_file_tokens: out-of-order processing reordered by time, within-segment kept") {
  // Simulate hetero / P5-bucketing: a later segment's tokens appended BEFORE an earlier
  // one. Distinct bases -> stable sort orders segments by time AND keeps each segment's
  // internal emission order.
  std::vector<Token> toks = { T("C",5.0), T("D",5.0), T("A",1.0), T("B",1.0) };
  auto segs = merge_file_tokens(toks, 1.0, 1000.0);
  REQUIRE(segs.size() == 2);
  CHECK(segs[0].text == u8"AB");   // earlier base first, order kept
  CHECK(segs[1].text == u8"CD");
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

// ---- <sil>/special-token filtering (int8-CTC leaks silence/special markers) ----
// int8-CTC emits silence/special tokens (e.g. "< sil >") that must NOT appear in the
// visible segment text. fp16-AED doesn't emit them, so it's unaffected.

TEST_CASE("SIL: '< sil >' tokens are stripped from segment text") {
  // Stream mirrors the real leak: DAY AFTER < sil > TO < sil > MOR ...
  std::vector<Token> toks = {
    T("DAY",0.0), T("AFTER",0.3), T("< sil >",0.6), T("TO",0.9),
    T("< sil >",1.2), T("MOR",1.5)
  };
  auto segs = merge_tokens(toks, 10.0, 30.0); // big gap -> single segment
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].text == "DAYAFTERTOMOR");      // no "< sil >" leaked
  CHECK(segs[0].text.find("sil") == std::string::npos);
}

TEST_CASE("SIL: '<sil>' / '<blank>' / empty / whitespace tokens are stripped") {
  std::vector<Token> toks = {
    T("hello",0.0), T("<sil>",0.2), T("<blank>",0.4), T("",0.6),
    T("   ",0.8), T("world",1.0)
  };
  auto segs = merge_tokens(toks, 10.0, 30.0);
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].text == "helloworld");
}

TEST_CASE("SIL: any angle-bracketed special token is stripped (conservative)") {
  std::vector<Token> toks = {
    T("a",0.0), T("<unk>",0.2), T("b",0.4), T("<s>",0.6), T("c",0.8)
  };
  auto segs = merge_tokens(toks, 10.0, 30.0);
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].text == "abc");
}

TEST_CASE("SIL: real content tokens with angle brackets in the middle are kept") {
  // Only tokens whose TRIMMED text is fully angle-bracketed (^<.*>$) are dropped.
  // A token like "a<b" or "x>y" is real content and must survive.
  std::vector<Token> toks = { T("a<b",0.0), T("x>y",0.5) };
  auto segs = merge_tokens(toks, 10.0, 30.0);
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].text == "a<bx>y");
}

TEST_CASE("SIL: a stream with no special tokens is unchanged") {
  std::vector<Token> toks = { T(u8"你",0.0),T(u8"好",0.3),T(u8"世",2.0),T(u8"界",2.2) };
  auto segs = merge_tokens(toks, 1.0, 30.0);
  REQUIRE(segs.size() == 2);
  CHECK(segs[0].text == u8"你好");
  CHECK(segs[1].text == u8"世界");
}

TEST_CASE("SIL: leading/trailing special tokens still segment + time correctly") {
  // A "< sil >" at the boundary must not become a segment's start time or text,
  // and merge/gap logic on the surviving real tokens must still work.
  std::vector<Token> toks = {
    T("< sil >",0.0), T("a",0.5), T("b",0.8),
    T("c",3.0), T("< sil >",3.3)
  };
  auto segs = merge_tokens(toks, 1.0, 30.0); // gap 0.8->3.0 = 2.2 > 1.0 -> split
  REQUIRE(segs.size() == 2);
  CHECK(segs[0].text == "ab");
  CHECK(segs[1].text == "c");
}
