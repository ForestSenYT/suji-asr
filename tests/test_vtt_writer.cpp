#include "doctest/doctest.h"
#include "core/output/vtt_writer.h"
#include "core/config.h"
#include <sstream>
using namespace suji;

TEST_CASE("vtt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"abc"; t.segments={s};
  EngineConfig cfg;
  std::string vtt = to_vtt(t, cfg);
  CHECK(vtt.rfind("WEBVTT\n\n",0)==0);
  CHECK(vtt.find("00:00:01.000 --> 00:00:02.500\nabc\n\n") != std::string::npos);
}

TEST_CASE("G5: vtt max=0 leaves text unwrapped") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五六七八九十";
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 0;
  std::string vtt = to_vtt(t, cfg);
  CHECK(vtt.find(u8"一二三四五六七八九十") != std::string::npos);
}

TEST_CASE("G5: vtt no leading space on wrapped continuation lines") {
  // "hello wo rld" with max=8: hard-break fires -> continuation must not start with space
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = "hello wo rld";
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 8;
  std::string vtt = to_vtt(t, cfg);
  auto tpos = vtt.find("00:00:00.000 --> 00:00:01.000\n");
  REQUIRE(tpos != std::string::npos);
  std::string body = vtt.substr(tpos + 30);
  std::string line;
  std::istringstream ss(body);
  bool any_leading_space = false;
  while (std::getline(ss, line)) {
    if (!line.empty() && line[0] == ' ') any_leading_space = true;
  }
  CHECK_FALSE(any_leading_space);
}

TEST_CASE("G5: vtt max=6 wraps 10-char zh string") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五六七八九十";
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 6;
  std::string vtt = to_vtt(t, cfg);
  // Find cue body after timecode line
  auto tpos = vtt.find("00:00:00.000 --> 00:00:01.000\n");
  REQUIRE(tpos != std::string::npos);
  std::string body = vtt.substr(tpos + 30);
  auto nl = body.find('\n');
  REQUIRE(nl != std::string::npos);
  std::string line1 = body.substr(0, nl);
  int cp = 0;
  for (unsigned char b : line1) if ((b & 0xC0u) != 0x80u) ++cp;
  CHECK(cp <= 6);
}
