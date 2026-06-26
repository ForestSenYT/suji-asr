// #3: default output location — write next to the SOURCE file when no output
// directory is chosen (matches the "（与源文件相同）" toolbar label).
//
// These standalone helpers mirror the UTF-8-safe stem/parent_dir/resolve_base
// logic in src/gui/engine_worker.cpp (the GUI is a separate build target not
// linked into the test binary, so we replicate the logic here — same pattern as
// test_stem_dedup.cpp). They operate on raw UTF-8 bytes (ASCII-transparent) so
// multibyte (Chinese) path components survive instead of being mangled through
// std::filesystem's narrow ANSI path.
#include "doctest/doctest.h"
#include <string>

static std::string stem(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name = name.substr(0, dot);
    return name;
}

static std::string parent_dir(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

// outDirStd empty -> next to source: parent_dir(input)/stem(input)
// outDirStd set   -> chosen dir:      outDirStd/stem(input)
static std::string resolve_base(const std::string& input, const std::string& outDirStd) {
    if (outDirStd.empty())
        return parent_dir(input) + "/" + stem(input);
    return outDirStd + "/" + stem(input);
}

TEST_CASE("default output: empty outDir writes next to the source file") {
    // The headline case from the task: an .mp4 with a Chinese stem next to source.
    CHECK(resolve_base("C:/foo/bar/\xe6\xb5\x8b\xe8\xaf\x95.mp4", "")
          == "C:/foo/bar/\xe6\xb5\x8b\xe8\xaf\x95");
    // NOT "out/测试" — base stays beside the input, not in a forced "out" dir.
    CHECK(resolve_base("C:/foo/bar/\xe6\xb5\x8b\xe8\xaf\x95.mp4", "")
          != "out/\xe6\xb5\x8b\xe8\xaf\x95");
}

TEST_CASE("default output: empty outDir with backslash separators (Windows path)") {
    CHECK(resolve_base("D:\\videos\\lecture.mkv", "") == "D:\\videos/lecture");
}

TEST_CASE("default output: empty outDir, bare filename -> current dir") {
    // No separator -> parent is "." (cwd), matching the source's own location.
    CHECK(resolve_base("clip.wav", "") == "./clip");
}

TEST_CASE("default output: chosen dir keeps the existing dir/stem behaviour") {
    CHECK(resolve_base("C:/foo/bar/video.mp4", "D:/out") == "D:/out/video");
    // Chinese stem preserved under a chosen dir too.
    CHECK(resolve_base("C:/foo/\xe8\xa7\x86\xe9\xa2\x91.mp4", "out")
          == "out/\xe8\xa7\x86\xe9\xa2\x91");
}

TEST_CASE("default output: stem strips only the final extension") {
    CHECK(resolve_base("C:/a/my.video.final.mp4", "") == "C:/a/my.video.final");
    CHECK(parent_dir("C:/a/b/c.txt") == "C:/a/b");
    CHECK(stem("C:/a/b/c.txt") == "c");
}
