// G6: output stem collision dedup
// Tests the dedup helper logic used in suji_cli and GUI engine_worker.
// (Mirrors suji_batch's existing dedup in batch_main.cpp.)
#include "doctest/doctest.h"
#include <set>
#include <string>

// Standalone dedup helper (mirrors what CLI & GUI should use).
// Given a candidate base string and a set of already-used bases, returns a
// unique base by appending _2, _3 ... on collision.
static std::string dedup_base(const std::string& base, std::set<std::string>& used) {
    std::string b = base;
    int n = 2;
    while (used.count(b)) {
        b = base + "_" + std::to_string(n++);
    }
    used.insert(b);
    return b;
}

TEST_CASE("G6: dedup_base: no collision returns original base") {
    std::set<std::string> used;
    CHECK(dedup_base("outdir/lecture", used) == "outdir/lecture");
    CHECK(used.count("outdir/lecture") == 1);
}

TEST_CASE("G6: dedup_base: collision produces _2") {
    std::set<std::string> used;
    used.insert("outdir/lecture");
    CHECK(dedup_base("outdir/lecture", used) == "outdir/lecture_2");
}

TEST_CASE("G6: dedup_base: two same-stem inputs get distinct bases") {
    std::set<std::string> used;
    std::string b1 = dedup_base("out/video", used);
    std::string b2 = dedup_base("out/video", used);
    CHECK(b1 == "out/video");
    CHECK(b2 == "out/video_2");
    CHECK(b1 != b2);
}

TEST_CASE("G6: dedup_base: three same-stem inputs produce _2 and _3") {
    std::set<std::string> used;
    std::string b1 = dedup_base("out/x", used);
    std::string b2 = dedup_base("out/x", used);
    std::string b3 = dedup_base("out/x", used);
    CHECK(b1 == "out/x");
    CHECK(b2 == "out/x_2");
    CHECK(b3 == "out/x_3");
}

TEST_CASE("G6: dedup_base: distinct stems don't interfere") {
    std::set<std::string> used;
    CHECK(dedup_base("out/a", used) == "out/a");
    CHECK(dedup_base("out/b", used) == "out/b");
    CHECK(dedup_base("out/a", used) == "out/a_2"); // only "a" is taken, not "b"
}
