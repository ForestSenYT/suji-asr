// T18: recursive folder scan — unit test for CLI & GUI directory expansion
// Creates a nested temp directory with a media file 2 levels deep and verifies
// recursive_directory_iterator finds it (mirrors batch_main.cpp directory expansion).
#include "doctest/doctest.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace fs = std::filesystem;

// Mirrors batch_main.cpp is_media() logic
static bool is_media_t18(const fs::path& p) {
    static const char* ext[] = {
        ".mp4", ".mkv", ".mov", ".flv", ".avi", ".webm", ".ts",
        ".m4a", ".mp3", ".wav", ".flac", ".aac", ".ogg", ".opus"
    };
    std::string e = p.extension().string();
    for (auto& ch : e) ch = (char)tolower((unsigned char)ch);
    for (auto x : ext) if (e == x) return true;
    return false;
}

// Mirrors the FIXED batch_main.cpp directory scan (recursive_directory_iterator)
static std::vector<std::string> collect_media_recursive(const fs::path& root) {
    std::vector<std::string> out;
    for (auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && is_media_t18(entry.path()))
            out.push_back(entry.path().string());
    }
    return out;
}

TEST_CASE("T18: recursive folder scan finds media file 2 levels deep") {
    // Build: tmpdir/subA/subB/test.wav
    fs::path root = fs::temp_directory_path() / "suji_test_t18_recursive";
    fs::path nested = root / "subA" / "subB";
    fs::create_directories(nested);

    fs::path wav = nested / "test.wav";
    { std::ofstream f(wav); f << "dummy"; }

    // Also a non-media file at root level that must be excluded
    fs::path txt = root / "readme.txt";
    { std::ofstream f(txt); f << "ignore"; }

    auto found = collect_media_recursive(root);
    CHECK(found.size() == 1);
    if (!found.empty()) {
        // Path must contain the nested file
        CHECK(found[0].find("test.wav") != std::string::npos);
    }

    // Cleanup
    fs::remove_all(root);
}

TEST_CASE("T18: recursive folder scan finds multiple media files at varying depths") {
    fs::path root = fs::temp_directory_path() / "suji_test_t18_multi";
    fs::create_directories(root / "level1" / "level2");
    fs::create_directories(root / "other");

    // Media at root level
    { std::ofstream f(root / "top.mp4"); f << "x"; }
    // Media one level deep
    { std::ofstream f(root / "level1" / "mid.mp3"); f << "x"; }
    // Media two levels deep
    { std::ofstream f(root / "level1" / "level2" / "deep.wav"); f << "x"; }
    // Non-media at level2
    { std::ofstream f(root / "level1" / "level2" / "notes.txt"); f << "x"; }

    auto found = collect_media_recursive(root);
    CHECK(found.size() == 3);

    // Cleanup
    fs::remove_all(root);
}
