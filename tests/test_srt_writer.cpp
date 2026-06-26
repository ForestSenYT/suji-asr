#include "doctest/doctest.h"
#include "core/output/srt_writer.h"
#include "core/config.h"
using namespace suji;

TEST_CASE("srt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"你好世界"; t.segments={s};
  EngineConfig cfg; // srt_max_chars_per_line = 0 (no wrap)
  std::string srt = to_srt(t, cfg);
  CHECK(srt == "1\n00:00:01,000 --> 00:00:02,500\n\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\n\n");
}

TEST_CASE("G5: srt max=0 leaves text unwrapped") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五六七八九十"; // 10 CJK chars
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 0;
  std::string srt = to_srt(t, cfg);
  // text should appear intact (no extra newline splitting the 10-char string)
  std::string cue_body = u8"一二三四五六七八九十";
  CHECK(srt.find(cue_body) != std::string::npos);
}

TEST_CASE("G5: srt max=6 wraps 10-char zh string into lines <=6 codepoints") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五六七八九十"; // 10 CJK chars, each 3 bytes
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 6;
  std::string srt = to_srt(t, cfg);
  // Find the cue body (after the timecode line)
  auto tpos = srt.find("00:00:00,000 --> 00:00:01,000\n");
  REQUIRE(tpos != std::string::npos);
  std::string body = srt.substr(tpos + 30); // after timecode line
  // body should contain a newline splitting the text before the trailing \n\n
  // Find first \n in body
  auto nl = body.find('\n');
  REQUIRE(nl != std::string::npos);
  std::string line1 = body.substr(0, nl);
  // Count UTF-8 codepoints in line1
  int cp = 0;
  for (unsigned char b : line1) if ((b & 0xC0u) != 0x80u) ++cp;
  CHECK(cp <= 6);
}

TEST_CASE("G5: srt multi-byte UTF-8 not split mid-character") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五"; // 5 CJK chars, 15 bytes
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 3;
  std::string srt = to_srt(t, cfg);
  // Verify each UTF-8 sequence in the output is well-formed
  const unsigned char* p   = reinterpret_cast<const unsigned char*>(srt.data());
  const unsigned char* end = p + srt.size();
  bool valid = true;
  while (p < end) {
    unsigned char b = *p;
    int len = (b < 0x80u) ? 1 : (b < 0xE0u) ? 2 : (b < 0xF0u) ? 3 : 4;
    for (int j = 1; j < len && p + j < end; ++j)
      if (((unsigned char)p[j] & 0xC0u) != 0x80u) { valid = false; break; }
    if (!valid) break;
    p += len;
  }
  CHECK(valid);
}
