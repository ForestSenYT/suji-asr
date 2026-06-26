// T9: write_outputs returns false on an unwritable path.
#include "doctest/doctest.h"
#include "core/output/writer_facade.h"
#include "core/config.h"
#include "core/types.h"
#include "core/utf8_file.h"
#include <cstdio>
#include <filesystem>
using namespace suji;
namespace fs = std::filesystem;

// Helper: build a minimal non-empty transcript
static Transcript make_transcript() {
    Transcript t;
    Segment s; s.start = 0.0; s.end = 1.0; s.text = "hello";
    Token tk; tk.text = "hello"; tk.start = 0.0;
    s.tokens = {tk};
    t.segments = {s};
    t.full_text = "hello";
    return t;
}

TEST_CASE("T9: write_outputs returns true on a valid path") {
    EngineConfig cfg;
    cfg.out_srt = true; cfg.out_vtt = false; cfg.out_json = false; cfg.out_md = false;
    Transcript t = make_transcript();
    std::string base = "test_wo_valid";
    bool ok = write_outputs(t, base, cfg, "title");
    CHECK(ok);
    std::remove((base + ".srt").c_str());
}

TEST_CASE("T9: write_outputs returns false when output dir does not exist") {
    EngineConfig cfg;
    cfg.out_srt = true; cfg.out_vtt = false; cfg.out_json = false; cfg.out_md = false;
    Transcript t = make_transcript();
    // Path under a nonexistent directory — write should fail
    std::string base = "nonexistent_dir_xyz/subdir/file";
    bool ok = write_outputs(t, base, cfg, "title");
    CHECK_FALSE(ok);
}

TEST_CASE("T9: write_outputs returns false when path is under a file (not a dir)") {
    // Create a regular file, then try to write under it as if it were a directory.
    EngineConfig cfg;
    cfg.out_srt = true; cfg.out_vtt = false; cfg.out_json = false; cfg.out_md = false;
    Transcript t = make_transcript();
    // Write a plain file first
    std::string blocker = "test_wo_blocker_file.txt";
    write_utf8_no_bom(blocker, "I am a file, not a dir");
    // Now try to use it as a directory component in the path
    std::string base = blocker + "/subfile";
    bool ok = write_outputs(t, base, cfg, "title");
    CHECK_FALSE(ok);
    std::remove(blocker.c_str());
}
