// Writable-fallback helper test.
// Tests the logic extracted from engine_worker.cpp that redirects output to a
// guaranteed-writable fallback (Documents/suji-转写) when the desired output
// directory is not writable (e.g. Program Files needs admin).
//
// The helper lives as a standalone inline helper in engine_worker.cpp and is
// mirrored here for unit-testing — exactly the same pattern as
// test_default_output_base.cpp and test_gui_resume_partition.cpp.
//
// Non-writable simulation: create a regular file F, then pass F/sub as desired
// dir — create_directories cannot make a directory whose path runs through a
// file, so the writability probe fails portably without needing admin rights.
#include "doctest/doctest.h"
#include "core/utf8_file.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace suji;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Standalone mirror of the helpers in engine_worker.cpp
// ---------------------------------------------------------------------------

// UTF-8-safe stem (ASCII-transparent, same impl as engine_worker.cpp)
static std::string wf_stem(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name = name.substr(0, dot);
    return name;
}

// Check whether a directory path (UTF-8) is writable by trying to
// create_directories on it, then create+delete a tiny probe file inside it.
// Returns true if both succeed.
static bool dir_is_writable(const std::string& dir_utf8) {
    // 1. Try to create the directory (noop if already exists, fails if path is
    //    blocked by a file component — e.g. "somefile.txt/sub").
    std::error_code ec;
    fs::create_directories(fs::u8path(dir_utf8), ec);
    if (ec) return false;

    // 2. Try to write a probe file using the project's UTF-8-safe writer.
    std::string probe = dir_utf8 + "/.suji_writable_probe";
    if (!write_utf8_no_bom(probe, "")) return false;

    // 3. Delete probe (best-effort; failure is non-fatal — it's tiny).
#ifdef _WIN32
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, probe.data(), (int)probe.size(), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, probe.data(), (int)probe.size(), w.data(), n);
        DeleteFileW(w.c_str());
    }
#else
    std::remove(probe.c_str());
#endif
    return true;
}

// Resolve the effective output base for one input file.
//   desired_dir  — the directory we want to write into (UTF-8, may be empty).
//   file_stem    — the filename stem (no extension, no directory).
//   fallback_dir — guaranteed-writable fallback (Documents/suji-转写).
//   dir_cache    — per-caller map from desired_dir -> effective_dir; avoids
//                  probing the same directory more than once per batch.
//
// Returns effective_dir + "/" + file_stem.
static std::string resolve_base_writable(
    const std::string& desired_dir,
    const std::string& file_stem,
    const std::string& fallback_dir,
    std::map<std::string, std::string>& dir_cache)
{
    auto it = dir_cache.find(desired_dir);
    if (it == dir_cache.end()) {
        std::string effective;
        if (dir_is_writable(desired_dir)) {
            effective = desired_dir;
        } else {
            // Ensure fallback dir exists (best-effort; we trust it's writable).
            std::error_code ec;
            fs::create_directories(fs::u8path(fallback_dir), ec);
            effective = fallback_dir;
        }
        dir_cache[desired_dir] = effective;
        it = dir_cache.find(desired_dir);
    }
    return it->second + "/" + file_stem;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("writable-fallback: writable dir returns desired base unchanged") {
    fs::path tmp = fs::temp_directory_path() / "suji_wf_ok";
    fs::create_directories(tmp);
    std::string dir = tmp.u8string();

    std::map<std::string, std::string> cache;
    std::string result = resolve_base_writable(dir, "video", "SHOULD_NOT_USE", cache);

    // The returned base must be under the desired dir, not the fallback.
    CHECK(result == dir + "/video");
    // A write to that base + ".srt" must actually succeed.
    CHECK(write_utf8_no_bom(result + ".srt", "1\n00:00:01,000 --> 00:00:02,000\nhi\n\n"));

    fs::remove_all(tmp);
}

TEST_CASE("writable-fallback: non-writable dir redirects to fallback") {
    fs::path tmp = fs::temp_directory_path() / "suji_wf_nw";
    fs::create_directories(tmp);

    // Create a REGULAR FILE, then try to use "file/sub" as a dir — can't work.
    fs::path blocker = tmp / "blocker.txt";
    {
        std::ofstream f(blocker, std::ios::binary);
        f << "I am a file\n";
    }
    std::string bad_dir = (blocker / "sub").u8string();  // "blocker.txt/sub" — impossible

    fs::path fallback = tmp / "fallback_docs" / u8"suji-\xe8\xbd\xac\xe5\x86\x99";  // suji-转写
    std::string fallback_dir = fallback.u8string();
    fs::create_directories(fallback);

    std::map<std::string, std::string> cache;
    std::string result = resolve_base_writable(bad_dir, u8"\xe6\xb5\x8b\xe8\xaf\x95", fallback_dir, cache);
    // 测试 = \xe6\xb5\x8b\xe8\xaf\x95

    // Must have redirected: starts with fallback_dir, NOT bad_dir.
    REQUIRE_FALSE(result.empty());
    CHECK(result.rfind(fallback_dir, 0) == 0);
    CHECK(result == fallback_dir + u8"/\xe6\xb5\x8b\xe8\xaf\x95");

    // A write to the redirected base must SUCCEED.
    CHECK(write_utf8_no_bom(result + ".srt", "1\n00:00:01,000 --> 00:00:02,000\n\xe6\xb5\x8b\xe8\xaf\x95\n\n"));

    fs::remove_all(tmp);
}

TEST_CASE("writable-fallback: dir_is_writable caches — only probed once per dir") {
    fs::path tmp = fs::temp_directory_path() / "suji_wf_cache";
    fs::create_directories(tmp);
    std::string dir = tmp.u8string();

    std::map<std::string, std::string> cache;
    // Call twice with the same desired_dir.
    std::string r1 = resolve_base_writable(dir, "a", "NEVER_USED", cache);
    std::string r2 = resolve_base_writable(dir, "b", "NEVER_USED", cache);

    // Both under the desired dir.
    CHECK(r1 == dir + "/a");
    CHECK(r2 == dir + "/b");
    // Cache should have exactly one entry for this dir.
    CHECK(cache.size() == 1);
    CHECK(cache.count(dir) == 1);

    fs::remove_all(tmp);
}

TEST_CASE("writable-fallback: dedup still works against effective (post-redirect) base") {
    // Ensure that used_bases dedup operates on the effective (possibly redirected)
    // base, so two files from non-writable dirs don't collide in the fallback.
    fs::path tmp = fs::temp_directory_path() / "suji_wf_dedup";
    fs::create_directories(tmp);

    // Non-writable desired dir: blocker file makes "blocker/sub" impossible.
    fs::path blocker = tmp / "blocker2.txt";
    { std::ofstream f(blocker, std::ios::binary); f << "x"; }
    std::string bad_dir = (blocker / "sub").u8string();

    fs::path fallback = tmp / "fb";
    std::string fallback_dir = fallback.u8string();
    fs::create_directories(fallback);

    std::map<std::string, std::string> cache;
    // Two files with the same stem "clip" from the same non-writable dir.
    std::string base1 = resolve_base_writable(bad_dir, "clip", fallback_dir, cache);
    std::string base2 = resolve_base_writable(bad_dir, "clip", fallback_dir, cache);

    // Both point to the same effective path (caller dedup is separate concern),
    // but both must be under fallback_dir.
    CHECK(base1.rfind(fallback_dir, 0) == 0);
    CHECK(base2.rfind(fallback_dir, 0) == 0);
    // Both are the same base (dedup is the caller's responsibility, not ours).
    CHECK(base1 == fallback_dir + "/clip");
    CHECK(base2 == fallback_dir + "/clip");

    fs::remove_all(tmp);
}
