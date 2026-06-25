#include "doctest/doctest.h"
#include "core/timestamp.h"
using namespace suji;
TEST_CASE("srt time format") {
  CHECK(format_srt_time(0.0)        == "00:00:00,000");
  CHECK(format_srt_time(1.5)        == "00:00:01,500");
  CHECK(format_srt_time(61.25)      == "00:01:01,250");
  CHECK(format_srt_time(3661.007)   == "01:01:01,007");
}
TEST_CASE("vtt time format") {
  CHECK(format_vtt_time(61.25)      == "00:01:01.250");
}
