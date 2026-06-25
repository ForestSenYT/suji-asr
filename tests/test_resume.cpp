#include "doctest/doctest.h"
#include "core/resume.h"
#include "core/utf8_file.h"
#include "core/config.h"
#include <cstdio>
using namespace suji;

TEST_CASE("transcript_complete: all outputs present + non-empty SRT") {
  EngineConfig c; // defaults: all 4 enabled
  std::string b="resume_tmp_done";
  write_utf8_no_bom(b+".srt", "1\n00:00:01,000 --> 00:00:02,000\nhi\n\n");
  write_utf8_no_bom(b+".vtt", "WEBVTT\n\n...");
  write_utf8_no_bom(b+".json", "{\"full_text\":\"hi\",\"segments\":[{\"start\":1.000}]}");
  write_utf8_no_bom(b+".md", "# x\n\n**[00:00:01]** hi\n\n");
  CHECK(transcript_complete(b, c));
  for(auto e:{".srt",".vtt",".json",".md"}) std::remove((b+e).c_str());
}

TEST_CASE("transcript_complete: empty SRT (no segments) -> not complete") {
  EngineConfig c; std::string b="resume_tmp_empty";
  write_utf8_no_bom(b+".srt", "");                  // empty = no segments
  write_utf8_no_bom(b+".vtt", "WEBVTT\n\n");
  write_utf8_no_bom(b+".json", "{\"full_text\":\"\",\"segments\":[]}");
  write_utf8_no_bom(b+".md", "# x\n\n");
  CHECK_FALSE(transcript_complete(b, c));
  for(auto e:{".srt",".vtt",".json",".md"}) std::remove((b+e).c_str());
}

TEST_CASE("transcript_complete: missing a file -> not complete") {
  EngineConfig c; std::string b="resume_tmp_missing";
  write_utf8_no_bom(b+".srt", "1\n...\nhi\n\n");      // only srt
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".srt").c_str());
}

TEST_CASE("transcript_complete: respects disabled outputs") {
  EngineConfig c; c.out_vtt=false; c.out_json=false; c.out_md=false; // only srt enabled
  std::string b="resume_tmp_srtonly";
  write_utf8_no_bom(b+".srt", "1\n...\nhi\n\n");
  CHECK(transcript_complete(b, c));                  // only srt required, and it's non-empty
  std::remove((b+".srt").c_str());
}
