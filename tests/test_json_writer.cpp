#include "doctest/doctest.h"
#include "core/output/json_writer.h"
using namespace suji;
TEST_CASE("json escape + structure") {
  Transcript t; t.full_text=u8"他说\"hi\"";
  Segment s; s.start=1.0; s.end=2.0; s.text=u8"你"; Token tk; tk.text=u8"你"; tk.start=1.0; s.tokens={tk};
  t.segments={s};
  std::string j = to_json(t);
  CHECK(j.find("\\\"hi\\\"") != std::string::npos);          // 引号被转义
  CHECK(j.find("\"start\":1") != std::string::npos);
  CHECK(j.find("\xe4\xbd\xa0") != std::string::npos);        // 中文原样 UTF-8
}
