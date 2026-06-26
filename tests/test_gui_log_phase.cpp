// GUI log-panel + per-file phase unit tests (Tasks B + C2).
//
// Both helpers are pure logic functions that don't need a Qt event loop:
//
//   1. log_level_color: mirrors MainWindow::logLineHtml's level→color mapping.
//      We test the color-selection rule and HTML-escape logic using std::string
//      so no Qt linkage is required. The real logLineHtml is kept in lockstep
//      with this mirror (comment references: "C2 color rules").
//
//   2. file_phase_str: mirrors the static filePhaseStr() helper in main_window.cpp.
//      Tests the segsTotal==0 → 解码中 / 0<done<total → 转写中 / done==total → ""
//      mapping that drives the per-file status column (Task B).

#include "doctest/doctest.h"
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Mirror of the HTML-escape step in logLineHtml (C2).
// Escapes <, >, &, " so paths/text don't break the HTML markup.
// ---------------------------------------------------------------------------
static std::string html_escape(const std::string& s) {
    std::string out; out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '<':  out += "&lt;";  break;
            case '>':  out += "&gt;";  break;
            case '&':  out += "&amp;"; break;
            case '"':  out += "&quot;"; break;
            default:   out += c;       break;
        }
    }
    return out;
}

// Mirror of C2 color-selection rules (kept in lockstep with logLineHtml):
//   ERR level                        -> "red"
//   OK level / msg contains 完成|成功|ok -> "green"
//   phase keywords (解码|切分|转写|fp16|provider|using) -> "blue"
//   default                          -> "default"
static std::string log_color_category(const std::string& level, const std::string& msg) {
    if (level == "ERR") return "red";

    // Case-insensitive "ok" check: lower the message
    std::string lmsg = msg;
    std::transform(lmsg.begin(), lmsg.end(), lmsg.begin(), [](unsigned char c){ return (char)std::tolower(c); });

    if (level == "OK"
        || msg.find(u8"完成") != std::string::npos
        || msg.find(u8"成功") != std::string::npos
        || lmsg.find("ok") != std::string::npos)
        return "green";

    if (msg.find(u8"解码") != std::string::npos
        || msg.find(u8"切分") != std::string::npos
        || msg.find(u8"转写") != std::string::npos
        || msg.find("fp16")     != std::string::npos
        || msg.find("provider") != std::string::npos
        || msg.find("using")    != std::string::npos)
        return "blue";

    return "default";
}

// Mirror of filePhaseStr() in main_window.cpp (Task B).
// Returns the display phase string derived from per-file segment counters.
static std::string file_phase_str(int segs_done, int segs_total) {
    if (segs_total == 0)    return u8"解码中";
    if (segs_done < segs_total) return u8"转写中";
    return "";  // done==total: let fileResult set the terminal state
}

// ---------------------------------------------------------------------------
// Tests: log color rules (C2)
// ---------------------------------------------------------------------------
TEST_CASE("log color: ERR level -> red regardless of message") {
    CHECK(log_color_category("ERR", "something failed") == "red");
    CHECK(log_color_category("ERR", u8"完成") == "red");   // ERR wins over 完成 keyword
    CHECK(log_color_category("ERR", "ok")     == "red");   // ERR wins over "ok"
}

TEST_CASE("log color: OK level or 完成/成功/ok in message -> green") {
    CHECK(log_color_category("OK",   "done")         == "green");
    CHECK(log_color_category("INFO", u8"完成: foo.mp4 (12 段)") == "green");
    CHECK(log_color_category("INFO", u8"成功写入")   == "green");
    CHECK(log_color_category("INFO", "some ok path") == "green");
    CHECK(log_color_category("INFO", "OK here")      == "green");  // case-insensitive
}

TEST_CASE("log color: phase keywords -> blue (but 完成 in msg -> green takes priority)") {
    CHECK(log_color_category("INFO", u8"解码: foo.mp4")             == "blue");
    // 切分完成 contains 完成 -> green wins over blue (completion keyword priority > phase)
    CHECK(log_color_category("INFO", u8"切分完成: bar.wav (5 段)") == "green");
    CHECK(log_color_category("INFO", u8"转写进度")                  == "blue");
    CHECK(log_color_category("INFO", "using fp16 AED model on GPU") == "blue");
    CHECK(log_color_category("INFO", "GUI engine: provider=cuda")   == "blue");
    // Pure phase lines without 完成/成功/ok -> blue
    CHECK(log_color_category("INFO", u8"解码: long_video.mp4")      == "blue");
}

TEST_CASE("log color: default for unclassified INFO lines") {
    CHECK(log_color_category("INFO", "total audio to transcribe: 120s") == "default");
    CHECK(log_color_category("INFO", "hetero split: CPU 40% / GPU 60%") == "default");
    CHECK(log_color_category("INFO", "resume: skip (done) /some/file")  == "default");
}

TEST_CASE("log HTML-escape: < > & \" in message do not break markup") {
    CHECK(html_escape("<tag>")       == "&lt;tag&gt;");
    CHECK(html_escape("a & b")      == "a &amp; b");
    CHECK(html_escape("\"quoted\"") == "&quot;quoted&quot;");
    CHECK(html_escape("C:\\Users\\foo\\bar.mp4") == "C:\\Users\\foo\\bar.mp4");  // no change
    // A path like <video>.mp4 must be escaped
    CHECK(html_escape("<video>.mp4") == "&lt;video&gt;.mp4");
}

// ---------------------------------------------------------------------------
// Tests: per-file phase mapping (Task B)
// ---------------------------------------------------------------------------
TEST_CASE("per-file phase: segsTotal==0 -> 解码中 (decode/VAD not done)") {
    CHECK(file_phase_str(0, 0) == u8"解码中");   // no segments pushed at all yet
    CHECK(file_phase_str(5, 0) == u8"解码中");   // guard: segsTotal==0 wins
}

TEST_CASE("per-file phase: 0 < segsDone < segsTotal -> 转写中") {
    CHECK(file_phase_str(1, 10)  == u8"转写中");
    CHECK(file_phase_str(5, 10)  == u8"转写中");
    CHECK(file_phase_str(9, 10)  == u8"转写中");
}

TEST_CASE("per-file phase: segsDone == segsTotal -> empty (terminal from fileResult)") {
    // All segments routed: the terminal state (完成/失败/取消) comes from
    // onWorkerFileResult, not from the progress callback.  phase returns "".
    CHECK(file_phase_str(10, 10) == "");
    CHECK(file_phase_str(0, 0)   != "");  // zero total is still 解码中, not terminal
}

TEST_CASE("per-file phase: non-terminal phases never contain terminal keywords") {
    // Make sure we never accidentally return 完成 or 失败 from the phase helper.
    for (int done = 0; done <= 5; ++done) {
        for (int total = 0; total <= 10; ++total) {
            if (total > 0 && done > total) continue;  // skip impossible
            std::string p = file_phase_str(done, total);
            CHECK(p.find(u8"完成") == std::string::npos);
            CHECK(p.find(u8"失败") == std::string::npos);
            CHECK(p.find(u8"取消") == std::string::npos);
        }
    }
}
