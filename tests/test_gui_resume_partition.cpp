// G7: GUI resume partition unit test.
// Tests the logic that partitions inputs into "already complete" vs "todo",
// mirroring engine_worker.cpp's G7 partition.
// Uses real transcript_complete() on temp files to confirm the partition works.
#include "doctest/doctest.h"
#include "core/resume.h"
#include "core/config.h"
#include "core/utf8_file.h"
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>
using namespace suji;
namespace fs = std::filesystem;

// Minimal stem extractor (mirrors engine_worker.cpp stem())
static std::string g7_stem(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name = name.substr(0, dot);
    return name;
}

// Minimal complete SRT (same as test_resume.cpp)
static const char* FULL_SRT = "1\n00:00:01,000 --> 00:00:02,000\nhi\n\n";

TEST_CASE("G7: resume partition splits inputs into skipped + todo") {
    fs::path outdir = fs::temp_directory_path() / "suji_g7_test";
    fs::create_directories(outdir);
    std::string out = (outdir / "").string();   // base dir string with trailing sep

    // Config: only SRT enabled (simplifies the complete check)
    EngineConfig cfg;
    cfg.out_srt  = true;
    cfg.out_vtt  = false;
    cfg.out_json = false;
    cfg.out_md   = false;

    // file_done.wav  -> has a complete SRT already in outdir
    // file_todo.wav  -> no SRT yet
    std::string done_path = "fake_dir/file_done.wav";
    std::string todo_path = "fake_dir/file_todo.wav";

    // Pre-create the output for done_path
    std::string done_base = out + g7_stem(done_path);
    write_utf8_no_bom(done_base + ".srt", FULL_SRT);

    // Partition (mirrors engine_worker.cpp G7 block)
    std::vector<std::string> inputs = {done_path, todo_path};
    std::vector<std::string> skipped, todo;
    for (const std::string& f : inputs) {
        std::string base = out + g7_stem(f);
        if (transcript_complete(base, cfg))
            skipped.push_back(f);
        else
            todo.push_back(f);
    }

    CHECK(skipped.size() == 1);
    CHECK(todo.size() == 1);
    if (!skipped.empty()) CHECK(skipped[0] == done_path);
    if (!todo.empty())    CHECK(todo[0]    == todo_path);

    // Cleanup
    fs::remove_all(outdir);
}

TEST_CASE("G7: resume partition: all done -> todo is empty") {
    fs::path outdir = fs::temp_directory_path() / "suji_g7_alldone";
    fs::create_directories(outdir);
    std::string out = (outdir / "").string();

    EngineConfig cfg;
    cfg.out_srt = true; cfg.out_vtt = false; cfg.out_json = false; cfg.out_md = false;

    std::vector<std::string> inputs = {"dir/a.mp4", "dir/b.mp4"};
    for (const std::string& f : inputs)
        write_utf8_no_bom(out + g7_stem(f) + ".srt", FULL_SRT);

    std::vector<std::string> skipped, todo;
    for (const std::string& f : inputs) {
        std::string base = out + g7_stem(f);
        if (transcript_complete(base, cfg)) skipped.push_back(f);
        else                               todo.push_back(f);
    }

    CHECK(skipped.size() == 2);
    CHECK(todo.empty());

    fs::remove_all(outdir);
}

TEST_CASE("G7: resume partition: fresh outdir -> all go to todo") {
    fs::path outdir = fs::temp_directory_path() / "suji_g7_fresh";
    fs::create_directories(outdir);
    std::string out = (outdir / "").string();

    EngineConfig cfg;
    cfg.out_srt = true; cfg.out_vtt = false; cfg.out_json = false; cfg.out_md = false;

    std::vector<std::string> inputs = {"dir/c.mp4", "dir/d.mp4"};
    std::vector<std::string> skipped, todo;
    for (const std::string& f : inputs) {
        std::string base = out + g7_stem(f);
        if (transcript_complete(base, cfg)) skipped.push_back(f);
        else                               todo.push_back(f);
    }

    CHECK(skipped.empty());
    CHECK(todo.size() == 2);

    fs::remove_all(outdir);
}
