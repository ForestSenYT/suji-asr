#include "doctest/doctest.h"
#include "core/resume.h"
#include "core/utf8_file.h"
#include "core/config.h"
#include <cstdio>
using namespace suji;

// A complete, valid SRT cue block (used by multiple tests below).
static const char* COMPLETE_SRT = "1\n00:00:01,000 --> 00:00:02,000\nhi\n\n";
static const char* COMPLETE_VTT = "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nhi\n\n";
static const char* COMPLETE_JSON = "{\"full_text\":\"hi\",\"segments\":[{\"start\":1.000}]}";
static const char* COMPLETE_MD   = "# x\n\n**[00:00:01]** hi\n\n";

TEST_CASE("transcript_complete: all outputs present + non-empty SRT") {
  EngineConfig c; // defaults: all 4 enabled
  std::string b="resume_tmp_done";
  write_utf8_no_bom(b+".srt",  COMPLETE_SRT);
  write_utf8_no_bom(b+".vtt",  COMPLETE_VTT);
  write_utf8_no_bom(b+".json", COMPLETE_JSON);
  write_utf8_no_bom(b+".md",   COMPLETE_MD);
  CHECK(transcript_complete(b, c));
  for(auto e:{".srt",".vtt",".json",".md"}) std::remove((b+e).c_str());
}

TEST_CASE("transcript_complete: empty SRT (no segments) -> not complete") {
  EngineConfig c; std::string b="resume_tmp_empty";
  write_utf8_no_bom(b+".srt", "");                          // empty = no segments
  write_utf8_no_bom(b+".vtt", "WEBVTT\n\n");               // no cue
  write_utf8_no_bom(b+".json", "{\"full_text\":\"\",\"segments\":[]}");
  write_utf8_no_bom(b+".md", "# x\n\n");
  CHECK_FALSE(transcript_complete(b, c));
  for(auto e:{".srt",".vtt",".json",".md"}) std::remove((b+e).c_str());
}

TEST_CASE("transcript_complete: missing a file -> not complete") {
  EngineConfig c; std::string b="resume_tmp_missing";
  write_utf8_no_bom(b+".srt", COMPLETE_SRT);  // only srt present; vtt/json/md missing
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".srt").c_str());
}

TEST_CASE("transcript_complete: respects disabled outputs") {
  EngineConfig c; c.out_vtt=false; c.out_json=false; c.out_md=false; // only srt enabled
  std::string b="resume_tmp_srtonly";
  write_utf8_no_bom(b+".srt", COMPLETE_SRT);
  CHECK(transcript_complete(b, c));  // only srt required, and it's a complete cue
  std::remove((b+".srt").c_str());
}

// ---- T16: New tests for robustness against truncated files ----

TEST_CASE("T16: truncated JSON (missing closing brace) -> not complete") {
  EngineConfig c; c.out_srt=false; c.out_vtt=false; c.out_md=false;
  std::string b="resume_tmp_trunc_json";
  // Truncated mid-write: has segment marker but no closing '}'
  write_utf8_no_bom(b+".json", "{\"full_text\":\"hi\",\"segments\":[{\"start\":1.0");
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".json").c_str());
}

TEST_CASE("T16: complete JSON -> complete") {
  EngineConfig c; c.out_srt=false; c.out_vtt=false; c.out_md=false;
  std::string b="resume_tmp_full_json";
  write_utf8_no_bom(b+".json", COMPLETE_JSON);
  CHECK(transcript_complete(b, c));
  std::remove((b+".json").c_str());
}

TEST_CASE("T16: truncated SRT (arrow line but no text) -> not complete") {
  EngineConfig c; c.out_vtt=false; c.out_json=false; c.out_md=false;
  std::string b="resume_tmp_trunc_srt";
  // Has the arrow but was truncated before writing the text line
  write_utf8_no_bom(b+".srt", "1\n00:00:01,000 --> 00:00:02,000\n");
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".srt").c_str());
}

TEST_CASE("T16: truncated SRT (has text but no trailing blank line) -> not complete") {
  EngineConfig c; c.out_vtt=false; c.out_json=false; c.out_md=false;
  std::string b="resume_tmp_trunc_srt2";
  // Has cue header + text but the double-newline terminator is missing
  write_utf8_no_bom(b+".srt", "1\n00:00:01,000 --> 00:00:02,000\nhi");
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".srt").c_str());
}

TEST_CASE("T16: complete SRT -> complete") {
  EngineConfig c; c.out_vtt=false; c.out_json=false; c.out_md=false;
  std::string b="resume_tmp_full_srt";
  write_utf8_no_bom(b+".srt", COMPLETE_SRT);
  CHECK(transcript_complete(b, c));
  std::remove((b+".srt").c_str());
}

TEST_CASE("T16: truncated VTT (missing arrow) -> not complete") {
  EngineConfig c; c.out_srt=false; c.out_json=false; c.out_md=false;
  std::string b="resume_tmp_trunc_vtt";
  // WEBVTT header only, no cues
  write_utf8_no_bom(b+".vtt", "WEBVTT\n\npartial text without arrow\n");
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".vtt").c_str());
}

TEST_CASE("T16: complete VTT -> complete") {
  EngineConfig c; c.out_srt=false; c.out_json=false; c.out_md=false;
  std::string b="resume_tmp_full_vtt";
  write_utf8_no_bom(b+".vtt", COMPLETE_VTT);
  CHECK(transcript_complete(b, c));
  std::remove((b+".vtt").c_str());
}

TEST_CASE("T16: truncated MD (has marker but no trailing blank line) -> not complete") {
  EngineConfig c; c.out_srt=false; c.out_vtt=false; c.out_json=false;
  std::string b="resume_tmp_trunc_md";
  // Has a segment marker but was truncated without the trailing newlines
  write_utf8_no_bom(b+".md", "# x\n\n**[00:00:01]** hello world");
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".md").c_str());
}

TEST_CASE("T16: complete MD -> complete") {
  EngineConfig c; c.out_srt=false; c.out_vtt=false; c.out_json=false;
  std::string b="resume_tmp_full_md";
  write_utf8_no_bom(b+".md", COMPLETE_MD);
  CHECK(transcript_complete(b, c));
  std::remove((b+".md").c_str());
}

TEST_CASE("T16: old heuristic false positive - truncated JSON with start marker -> not complete") {
  // The OLD heuristic: file_contains(json, "\"start\":") -> would return TRUE for truncated files.
  // The new structural check requires last char to be '}': this must return FALSE.
  EngineConfig c; c.out_srt=false; c.out_vtt=false; c.out_md=false;
  std::string b="resume_tmp_oldpos_json";
  write_utf8_no_bom(b+".json",
    "{\"full_text\":\"hi\",\"segments\":[{\"start\":1.0,\"end\":2.0,\"text\":\"hi\"");
  // Old heuristic: finds "\"start\":" -> true (WRONG). New: no closing '}' -> false (CORRECT).
  CHECK_FALSE(transcript_complete(b, c));
  std::remove((b+".json").c_str());
}
