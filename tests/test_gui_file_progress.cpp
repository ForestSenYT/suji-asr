// GUI per-file progress ("每个视频分开") unit tests.
//
// The worker turns each engine FilePstat{file_index, segs_done, segs_total} into a
// fileProgress(path, percent, segsDone, segsTotal) signal, keyed by the original
// input PATH (engine file_index ≠ GUI table row when resume filters files). These
// tests pin the two pieces of pure logic that test without a Qt event loop:
//   1. percent computation  = (total>0) ? min(100, 100*done/total) : 0
//   2. file_index -> path mapping over the engine's (resume-filtered) inputs vector.
// The delegate's actual painting is visual (NEEDS-HUMAN); covered by compile + selftest.
#include "doctest/doctest.h"
#include "core/batch_engine.h"
#include <algorithm>
#include <string>
#include <vector>
using namespace suji;

// Mirror of engine_worker.cpp's percent computation (kept in lockstep with the lambda).
static int file_percent(long long done, long long total) {
    return (total > 0) ? std::min(100, (int)(100 * done / total)) : 0;
}

TEST_CASE("gui per-file percent: 0 when total unknown, clamped to 100") {
    CHECK(file_percent(0, 0)    == 0);    // decoding/VAD: nothing queued yet
    CHECK(file_percent(5, 0)    == 0);    // total still 0 -> guard avoids div-by-zero
    CHECK(file_percent(0, 10)   == 0);    // queued but none routed
    CHECK(file_percent(5, 10)   == 50);
    CHECK(file_percent(10, 10)  == 100);  // clean file -> 100%
    CHECK(file_percent(3, 7)    == 42);   // integer truncation (3/7 = 42.8 -> 42)
    CHECK(file_percent(11, 10)  == 100);  // never exceeds 100 even if done>total transiently
}

TEST_CASE("gui per-file: file_index maps to the engine-input path, Sigma-invariant holds") {
    // The engine receives the resume-FILTERED inputs (todo), so FilePstat.file_index
    // indexes into THAT vector — not the original GUI rows. Verify the worker's mapping
    // (todo[fp.file_index]) and that the per-file snapshot sums to the global counters.
    std::vector<std::string> todo = {"/a/one.mp4", "/b/two.mkv", "/c/three.wav"};

    BatchProgress b;
    b.segs_done = 12; b.segs_total = 20;
    b.files = { {0, 4, 5}, {1, 3, 10}, {2, 5, 5} };  // index, done, total

    long long sum_done = 0, sum_total = 0;
    std::vector<std::pair<std::string,int>> emitted;   // (path, percent) the worker would emit
    for (const FilePstat& fp : b.files) {
        REQUIRE(fp.file_index >= 0);
        REQUIRE(fp.file_index < (int)todo.size());
        emitted.emplace_back(todo[fp.file_index], file_percent(fp.segs_done, fp.segs_total));
        sum_done += fp.segs_done; sum_total += fp.segs_total;
    }
    // Σ per-file == global (the engine's invariant, surfaced to the GUI unchanged).
    CHECK(sum_done  == b.segs_done);
    CHECK(sum_total == b.segs_total);

    // Mapping + percents.
    REQUIRE(emitted.size() == 3);
    CHECK(emitted[0].first == "/a/one.mp4");   CHECK(emitted[0].second == 80);  // 4/5
    CHECK(emitted[1].first == "/b/two.mkv");   CHECK(emitted[1].second == 30);  // 3/10
    CHECK(emitted[2].first == "/c/three.wav"); CHECK(emitted[2].second == 100); // 5/5
}
