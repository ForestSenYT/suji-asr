// GUI row-output-dir resolution unit tests (E).
//
// MainWindow::rowOutputDir(path, chosenDir) resolves the folder a completed
// row's outputs were (likely) written to, for the "打开输出文件夹" action. It
// mirrors EngineWorker::run's desired_dir:
//     chosenDir.isEmpty() ? parent_dir(path) : chosenDir
// (the not-writable -> Documents fallback is intentionally NOT modeled; it only
// fires on read-only source dirs and the action falls back to a log line then.)
//
// suji_tests links only suji_core (no Qt), so we test a std::string MIRROR of
// that pure logic kept in lockstep with the QString implementation — the same
// pattern test_gui_log_phase.cpp uses for filePhaseStr / log color rules.
#include "doctest/doctest.h"
#include <string>

// Mirror of the UTF-8-safe parent_dir used by the worker + MainWindow::rowOutputDir
// (QFileInfo::absolutePath on a slash-normalized path). Operates on raw bytes so
// Chinese path components survive.
static std::string parent_dir(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

// Mirror of MainWindow::rowOutputDir.
static std::string row_output_dir(const std::string& path, const std::string& chosen) {
    if (!chosen.empty()) return chosen;
    return parent_dir(path);
}

TEST_CASE("row output dir: chosen dir wins when set") {
    CHECK(row_output_dir("/src/a.mp4", "/out")        == "/out");
    CHECK(row_output_dir("C:/v/a.mkv", "D:/subs")     == "D:/subs");
    // Even for a bare filename, an explicit chosen dir is used verbatim.
    CHECK(row_output_dir("a.mp4", "/out")             == "/out");
}

TEST_CASE("row output dir: empty chosen -> next to the source file") {
    CHECK(row_output_dir("/src/a.mp4", "")            == "/src");
    CHECK(row_output_dir("C:/videos/clip.mkv", "")    == "C:/videos");
    CHECK(row_output_dir("C:\\videos\\clip.mkv", "")  == "C:\\videos");  // backslash sep
}

TEST_CASE("row output dir: bare filename, empty chosen -> cwd '.'") {
    CHECK(row_output_dir("a.mp4", "")                 == ".");
}

TEST_CASE("row output dir: UTF-8 (Chinese) path components survive") {
    // 采访片段.wav next to 视频 dir — bytes are preserved, no ANSI mangling.
    CHECK(row_output_dir(u8"D:/视频/采访片段.wav", "")  == u8"D:/视频");
    CHECK(row_output_dir(u8"D:/视频/采访片段.wav", u8"E:/字幕") == u8"E:/字幕");
}
