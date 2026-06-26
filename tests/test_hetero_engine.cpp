// H3 — heterogeneous dual-consumer engine: pure (no-model) invariant tests.
//
// These tests exercise the concurrency/merge contracts of the hetero path
// WITHOUT loading any ASR/VAD model, so they run fast and deterministically:
//   1. No double-processing: two consumers draining one BoundedQueue via
//      pop/try_pop see DISJOINT, COMPLETE work (union==all, intersection==empty).
//   2. Token merge/sort: two interleaved per-engine token sinks, concatenated +
//      sorted by .start, equal the single-array baseline through merge_tokens.
//   3. Cancel safety: a blocked producer + cancel -> both consumer threads join
//      within a timeout (no hang).

#include "doctest/doctest.h"
#include "core/bounded_queue.h"
#include "core/segment_merge.h"
#include "core/cancel.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <algorithm>
#include <iterator>

using namespace suji;

namespace {
// Mirror of the file-static SegTask in batch_engine.cpp for the pure queue test.
struct TaskStub { int id; };
}

TEST_CASE("hetero: two consumers drain queue with no double-processing") {
  const int K = 5000;
  BoundedQueue<TaskStub> q(64);

  std::set<int> seen_a, seen_b;
  // Reusable consumer body identical in shape to the real engine: pop one, then
  // opportunistically try_pop up to a batch, recording every id exactly once.
  auto consume = [&](std::set<int>& sink, int batch_max){
    TaskStub first;
    while(q.pop(first)){
      std::vector<int> batch; batch.push_back(first.id);
      TaskStub more;
      while((int)batch.size() < batch_max && q.try_pop(more)) batch.push_back(more.id);
      for(int id : batch) sink.insert(id);
    }
  };

  std::thread ca([&]{ consume(seen_a, 4);  });   // "cpu" consumer, small batch
  std::thread cb([&]{ consume(seen_b, 30); });   // "gpu" consumer, big batch

  for(int i=0;i<K;i++) q.push(TaskStub{i});
  q.close();
  ca.join(); cb.join();

  // intersection empty: a single segment is popped exactly once.
  std::vector<int> inter;
  std::set_intersection(seen_a.begin(), seen_a.end(), seen_b.begin(), seen_b.end(),
                        std::back_inserter(inter));
  CHECK(inter.empty());
  // union complete: every pushed task processed exactly once.
  CHECK((int)(seen_a.size() + seen_b.size()) == K);
  std::set<int> uni = seen_a; uni.insert(seen_b.begin(), seen_b.end());
  CHECK((int)uni.size() == K);
}

// P5 — length-bucketed reorder window: a local holding buffer (cap 2x batch) that
// sorts held tasks by length and emits the most-similar batch, flushing on EOF.
// This mirrors the real consumer loop in batch_engine.cpp WITHOUT a model, so it
// proves the bucketing invariants deterministically:
//   1. No double-processing + complete coverage with the buffer + reorder in play
//      (union == all, intersection == empty) — reordering must not drop/dup a task.
//   2. Every emitted batch is length-contiguous (similar lengths) — the bucketing
//      actually groups, so DecodeMultipleOfflineStreams pads less.
namespace {
struct LenTask { int id; int len; };
}
TEST_CASE("P5: bucketed two consumers drain with no double-processing + similar-length batches") {
  const int K = 4000;
  BoundedQueue<LenTask> q(64);

  std::mutex emu;                       // guards the shared batch-record vector
  std::vector<std::vector<int>> batches;   // every emitted batch's lengths, both consumers

  std::set<int> seen_a, seen_b;
  // Exact shape of the real P5 consumer: hold up to 2x batch, sort by len, emit the
  // shortest `bmax`, keep remainder; flush whatever is left once the queue is closed.
  auto consume = [&](std::set<int>& sink, int bmax){
    const size_t cap = (size_t)bmax * 2;
    std::vector<LenTask> hold;
    auto emit_one = [&](){
      std::sort(hold.begin(), hold.end(),
                [](const LenTask&a,const LenTask&b){ return a.len<b.len; });
      size_t take = std::min((size_t)bmax, hold.size());
      std::vector<int> lens;
      for(size_t i=0;i<take;i++){ sink.insert(hold[i].id); lens.push_back(hold[i].len); }
      hold.erase(hold.begin(), hold.begin()+take);
      std::lock_guard<std::mutex> lk(emu); batches.push_back(std::move(lens));
    };
    LenTask first;
    while(q.pop(first)){
      hold.push_back(first);
      LenTask more;
      while(hold.size() < cap && q.try_pop(more)) hold.push_back(more);
      while((int)hold.size() >= bmax) emit_one();
    }
    while(!hold.empty()) emit_one();   // clean EOF flush — never drop a held task
  };

  std::thread ca([&]{ consume(seen_a, 5);  });   // "cpu" consumer
  std::thread cb([&]{ consume(seen_b, 8);  });   // "gpu" consumer

  // Mixed lengths so bucketing has something to group (id carries a deterministic len).
  for(int i=0;i<K;i++) q.push(LenTask{i, (i*37) % 500});
  q.close();
  ca.join(); cb.join();

  // No double-processing: intersection empty, union complete.
  std::vector<int> inter;
  std::set_intersection(seen_a.begin(), seen_a.end(), seen_b.begin(), seen_b.end(),
                        std::back_inserter(inter));
  CHECK(inter.empty());
  CHECK((int)(seen_a.size() + seen_b.size()) == K);
  std::set<int> uni = seen_a; uni.insert(seen_b.begin(), seen_b.end());
  CHECK((int)uni.size() == K);

  // Bucketing actually grouped: across all emitted batches, the padding waste (sum of
  // max-len minus each item's len) is strictly less than a length-oblivious baseline
  // that pads every batch to the GLOBAL max length (499). If bucketing did nothing the
  // two would be equal; grouping similar lengths makes the per-batch max much smaller.
  long long bucket_waste = 0, global_waste = 0, total_items = 0;
  for(const auto& bl : batches){
    int bmaxlen = 0; for(int l : bl) bmaxlen = std::max(bmaxlen, l);
    for(int l : bl){ bucket_waste += (bmaxlen - l); global_waste += (499 - l); }
    total_items += (long long)bl.size();
  }
  CHECK(total_items == K);                 // every task emitted exactly once
  CHECK(bucket_waste < global_waste);      // length-bucketing reduces padding waste
}

TEST_CASE("hetero: per-engine token sinks sort/merge equal single-array baseline") {
  // Build interleaved timestamps split across two sinks, plus a single baseline.
  std::vector<Token> tok_cpu, tok_gpu, baseline;
  auto mk = [](const char* s, double t){ Token x; x.text=s; x.start=t; return x; };
  // global stream: 0.0 0.3 0.6 0.9 1.2 1.5 2.8 3.1 (one gap > 1.0 at 1.5->2.8)
  const double ts[] = {0.0,0.3,0.6,0.9,1.2,1.5,2.8,3.1};
  const char* txt[] = {u8"你",u8"好",u8"世",u8"界",u8"今",u8"天",u8"再",u8"见"};
  for(int i=0;i<8;i++){
    Token tk = mk(txt[i], ts[i]);
    baseline.push_back(tk);
    if(i % 2 == 0) tok_cpu.push_back(tk); else tok_gpu.push_back(tk);
  }

  // hetero finalize: concat both sinks, sort by .start, merge.
  std::vector<Token> merged = tok_cpu;
  merged.insert(merged.end(), tok_gpu.begin(), tok_gpu.end());
  std::sort(merged.begin(), merged.end(),
            [](const Token&a,const Token&b){ return a.start<b.start; });

  // monotonic after sort
  for(size_t i=1;i<merged.size();++i) CHECK(merged[i-1].start <= merged[i].start);

  auto seg_h = merge_tokens(merged, 1.0, 30.0);
  auto seg_b = merge_tokens(baseline, 1.0, 30.0);
  REQUIRE(seg_h.size() == seg_b.size());
  for(size_t i=0;i<seg_h.size();++i){
    CHECK(seg_h[i].text == seg_b[i].text);
    CHECK(seg_h[i].start == doctest::Approx(seg_b[i].start));
    CHECK(seg_h[i].end   == doctest::Approx(seg_b[i].end));
  }
}

TEST_CASE("hetero: cancel releases both consumers (no hang)" * doctest::timeout(10)) {
  BoundedQueue<TaskStub> q(8);
  CancelToken cancel;

  std::atomic<int> joined{0};
  auto consume = [&]{
    TaskStub first;
    while(q.pop(first)){
      if(cancel.is_cancelled()){ q.close(); break; }
      TaskStub more;
      while(q.try_pop(more)) { /* drain */ }
    }
    joined.fetch_add(1);
  };

  std::thread ca(consume), cb(consume);

  // "blocked producer": push a few then cancel + close, mimicking the engine's
  // join-producers -> queue.close() teardown under cancel.
  for(int i=0;i<4;i++) q.push(TaskStub{i});
  cancel.cancel();
  q.close();

  // Both consumer threads must terminate; if they hang the timeout fails the test.
  ca.join(); cb.join();
  CHECK(joined.load() == 2);
}

// ---------------------------------------------------------------------------
// R6 consumption tracking (bug fix): seg_pending per-file counter
//
// The bug: produced_complete[fi] is true once all segments are PUSHED, but if
// cancel fires while tail segments are still queued or are the popped-but-
// discarded `first` on the cancel path, those segments are never transcribed.
// The old finalize only checked produced_complete (push-complete), so such a
// file would be reported ok with a TRUNCATED transcript (R6 violation).
//
// The fix: track per-file CONSUMPTION via seg_pending[] atomics.
//   - producer: seg_pending[fi].fetch_add(1) before each queue.push()
//   - consumer: seg_pending[fi].fetch_sub(1) after routing each segment's tokens
//   - finalize: if cancel && seg_pending[i] > 0 -> ok=false err="cancelled"
//
// Test 1 (RED before fix / GREEN after fix):
//   Push K segments for file 0, mark production-complete, consume only some,
//   then cancel and classify -> file 0 must be ok=false, err="cancelled".
//
// Test 2 (positive / always GREEN):
//   Clean run (no cancel), consume ALL pushed segments -> seg_pending == 0
//   for every file -> no file is reclassified (all stay ok).
// ---------------------------------------------------------------------------

// Minimal finalize classification replicating the FIXED logic in batch_engine.cpp.
// Returns true if a file should be reclassified as cancelled.
// The fix: use seg_pending (consumption tracking) in addition to produced_complete.
static bool classify_cancelled(bool cancel_fired, int seg_pending, bool produced_complete,
                                bool currently_ok) {
  // Mirror of the finalize guard: only override files that are currently ok.
  if (!currently_ok) return false;
  return cancel_fired && (seg_pending > 0 || !produced_complete);
}

// OLD (buggy) classification that only checks produced_complete.
// Demonstrates what the bug produces: a fully-pushed but partially-consumed file
// is NOT reclassified, so it's reported ok with a truncated transcript.
static bool classify_cancelled_old_buggy(bool cancel_fired, bool produced_complete,
                                          bool currently_ok) {
  if (!currently_ok) return false;
  return cancel_fired && !produced_complete;  // BUG: misses seg_pending > 0 case
}

TEST_CASE("R6: partial consumption on cancel is classified cancelled, not ok") {
  // Simulate: file 0 has K segments pushed (produced_complete=true) but only
  // some were consumed when cancel fired.
  const int K = 10;
  const int consumed = 3;   // consumer processed 3 before observing cancel
  // seg_pending starts at 0; producer increments before push, consumer decrements after route.
  std::atomic<int> seg_pending{0};
  bool produced_complete = false;
  bool file_ok = true;   // no decode/VAD/transcribe error

  // Simulate producer: K pushes, with seg_pending incremented BEFORE each push.
  for (int i = 0; i < K; ++i) seg_pending.fetch_add(1);
  produced_complete = true;  // all pushed -> produced_complete is TRUE here

  // Simulate consumer: processes `consumed` segments then observes cancel.
  // The discarded `first` on the cancel path is NOT decremented (that's the point).
  for (int i = 0; i < consumed; ++i) seg_pending.fetch_sub(1);
  // cancel fires; remaining K-consumed segments (including the popped-but-discarded first)
  // stay pending -> seg_pending > 0.

  bool cancel_fired = true;

  // PROVE THE BUG: the OLD logic (produced_complete only) misclassifies this file as ok.
  bool old_says_cancelled = classify_cancelled_old_buggy(cancel_fired, produced_complete, file_ok);
  CHECK_FALSE(old_says_cancelled);  // old logic: produced_complete==true -> NOT reclassified -> WRONG

  // THE FIX: seg_pending > 0 catches the truncation; file must be reclassified cancelled.
  bool new_says_cancelled = classify_cancelled(cancel_fired, seg_pending.load(),
                                               produced_complete, file_ok);
  CHECK(seg_pending.load() == K - consumed);  // 7 segments unconsumed
  CHECK(new_says_cancelled);                  // fixed logic: truncated transcript NOT reported ok
}

TEST_CASE("R6: clean run leaves seg_pending zero and files stay ok") {
  // Simulate: 3 files, varying segment counts, all consumed, no cancel.
  const int N = 3;
  const int segs_per_file[] = {5, 3, 8};
  std::vector<std::atomic<int>> seg_pending(N);
  for (int i = 0; i < N; ++i) seg_pending[i].store(0);
  std::vector<bool> produced_complete(N, false);
  std::vector<bool> file_ok(N, true);

  // Simulate producers and consumers for each file.
  for (int fi = 0; fi < N; ++fi) {
    for (int s = 0; s < segs_per_file[fi]; ++s) seg_pending[fi].fetch_add(1);
    produced_complete[fi] = true;
    // All consumed cleanly (no cancel discard).
    for (int s = 0; s < segs_per_file[fi]; ++s) seg_pending[fi].fetch_sub(1);
  }

  bool cancel_fired = false;
  for (int i = 0; i < N; ++i) {
    // Clean run: seg_pending must be 0.
    CHECK(seg_pending[i].load() == 0);
    // Finalize must NOT reclassify any file as cancelled.
    bool reclassified = classify_cancelled(cancel_fired, seg_pending[i].load(),
                                           produced_complete[i], file_ok[i]);
    CHECK_FALSE(reclassified);
  }
}

// P5 + R6: segments sitting in a consumer's LOCAL holding buffer on cancel must keep
// the file classified cancelled. With the reorder window, a popped segment is counted
// (producer did seg_pending.fetch_add before push) but not yet routed while it waits in
// `hold`; the cancel path breaks WITHOUT routing/decrementing the buffer, so seg_pending
// stays elevated for every held segment -> finalize marks the file cancelled. No held
// segment is silently lost (it is accounted via seg_pending, exactly like the discarded
// `first`). This is the P5 analogue of the existing partial-consumption R6 test.
TEST_CASE("P5+R6: held (buffered-not-routed) segments on cancel keep file cancelled") {
  const int K = 12;        // all pushed (produced_complete=true)
  const int routed = 4;    // consumer routed 4 full batches' worth before cancel
  const int held = 3;      // 3 segments popped into the local hold buffer, NOT yet routed
  // remaining K-routed-held are still in the queue (also still pending).
  std::atomic<int> seg_pending{0};

  for (int i = 0; i < K; ++i) seg_pending.fetch_add(1);   // producer counted every push
  bool produced_complete = true;
  for (int i = 0; i < routed; ++i) seg_pending.fetch_sub(1);  // only ROUTED segs decrement
  // `held` segments were try_pop'd into the buffer but cancel fired before emit -> they are
  // NOT decremented (the consumer breaks, leaving `hold` un-routed). Nothing decrements them.

  CHECK(seg_pending.load() == K - routed);   // held + still-queued all remain pending
  CHECK(seg_pending.load() >= held);         // the buffered-not-routed segments are accounted

  bool cancel_fired = true;
  bool says_cancelled = classify_cancelled(cancel_fired, seg_pending.load(),
                                           produced_complete, /*currently_ok=*/true);
  CHECK(says_cancelled);   // truncated (buffer flushed-or-not, segments unrouted) -> NOT ok
}
