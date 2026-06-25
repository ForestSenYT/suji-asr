#include "doctest/doctest.h"
#include "core/output/md_writer.h"
using namespace suji;
TEST_CASE("md basic") {
  Transcript t; Segment s; s.start=61.0; s.text=u8"讲课内容"; t.segments={s};
  std::string md = to_markdown(t, u8"第一课");
  CHECK(md.rfind(u8"# 第一课",0)==0);
  CHECK(md.find(u8"**[00:01:01]** 讲课内容") != std::string::npos);
}
