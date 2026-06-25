#include "doctest/doctest.h"
#include "core/output/vtt_writer.h"
using namespace suji;
TEST_CASE("vtt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"abc"; t.segments={s};
  std::string vtt = to_vtt(t);
  CHECK(vtt.rfind("WEBVTT\n\n",0)==0);
  CHECK(vtt.find("00:00:01.000 --> 00:00:02.500\nabc\n\n") != std::string::npos);
}
