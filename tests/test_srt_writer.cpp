#include "doctest/doctest.h"
#include "core/output/srt_writer.h"
using namespace suji;
TEST_CASE("srt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"你好世界"; t.segments={s};
  std::string srt = to_srt(t);
  CHECK(srt == "1\n00:00:01,000 --> 00:00:02,500\n\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\n\n");
}
