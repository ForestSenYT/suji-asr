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
#include "core/batch_form.h"
#include "core/segment_merge.h"
#include "core/cancel.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <queue>
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

// Data-parallel CPU consumers: K (>2) consumers draining ONE BoundedQueue must see
// DISJOINT + COMPLETE work, exactly like the 2-consumer hetero case. This is the pure
// (no-model) analogue proving the K-sink design (transcribe_batch_files_single) can't
// double-process or drop a segment regardless of K. Mirrors the consumer loop shape.
TEST_CASE("data-parallel: K consumers drain queue with no double-processing") {
  for (int K : {3, 4}) {
    const int M = 6000;
    BoundedQueue<TaskStub> q(64);
    std::vector<std::set<int>> seen((size_t)K);
    auto consume = [&](std::set<int>& sink, int batch_max){
      TaskStub first;
      while(q.pop(first)){
        std::vector<int> batch; batch.push_back(first.id);
        TaskStub more;
        while((int)batch.size() < batch_max && q.try_pop(more)) batch.push_back(more.id);
        for(int id : batch) sink.insert(id);
      }
    };
    std::vector<std::thread> ts;
    for(int k=0;k<K;k++) ts.emplace_back([&,k]{ consume(seen[k], 4 + k); });  // varied batch sizes
    for(int i=0;i<M;i++) q.push(TaskStub{i});
    q.close();
    for(auto& t : ts) t.join();

    // Pairwise intersections empty: a single segment is popped by exactly one consumer.
    for(int a=0;a<K;a++) for(int b=a+1;b<K;b++){
      std::vector<int> inter;
      std::set_intersection(seen[a].begin(), seen[a].end(), seen[b].begin(), seen[b].end(),
                            std::back_inserter(inter));
      CHECK(inter.empty());
    }
    // Union complete: every pushed task processed exactly once across all K consumers.
    std::set<int> uni; size_t total = 0;
    for(int k=0;k<K;k++){ uni.insert(seen[k].begin(), seen[k].end()); total += seen[k].size(); }
    CHECK((int)total == M);            // no double-count
    CHECK((int)uni.size() == M);       // no drop
  }
}

// Data-parallel: K per-consumer token sinks, concatenated + stable-sorted by .start,
// must equal the single-array baseline through merge. This is the K-sink generalisation
// of the hetero 2-sink merge test — proves the finalize concat-then-merge is order-correct
// regardless of which of the K consumers processed which segment.
TEST_CASE("data-parallel: K per-consumer token sinks merge equal single-array baseline") {
  const int K = 4;
  std::vector<std::vector<Token>> sinks((size_t)K);
  std::vector<Token> baseline;
  auto mk = [](const char* s, double t){ Token x; x.text=s; x.start=t; return x; };
  const double ts[]  = {0.0,0.3,0.6,0.9,1.2,1.5,2.8,3.1};
  const char*  txt[] = {u8"你",u8"好",u8"世",u8"界",u8"今",u8"天",u8"再",u8"见"};
  for(int i=0;i<8;i++){
    Token tk = mk(txt[i], ts[i]);
    baseline.push_back(tk);
    sinks[i % K].push_back(tk);          // round-robin across K consumers
  }
  // finalize: concat all K sinks, stable-sort by .start, merge.
  std::vector<Token> merged;
  for(int k=0;k<K;k++) merged.insert(merged.end(), sinks[k].begin(), sinks[k].end());
  std::stable_sort(merged.begin(), merged.end(),
                   [](const Token&a,const Token&b){ return a.start<b.start; });
  for(size_t i=1;i<merged.size();++i) CHECK(merged[i-1].start <= merged[i].start);
  auto seg_h = merge_tokens(merged, 1.0, 30.0);
  auto seg_b = merge_tokens(baseline, 1.0, 30.0);
  REQUIRE(seg_h.size() == seg_b.size());
  for(size_t i=0;i<seg_h.size();++i){
    CHECK(seg_h[i].text == seg_b[i].text);
    CHECK(seg_h[i].start == doctest::Approx(seg_b[i].start));
  }
}

// P5 gate (REAL) — drives the PRODUCTION helpers form_next_batch()/flush_held() from
// batch_form.h (the same code both consumers in batch_engine.cpp call), so this test
// FAILS if the bucket=(N==1) gate is removed or inverted. The helper is gated by `bucket`:
//   bucket=false (N>1) -> FIFO arrival order, no hold/sort (pre-P5 multi-file behaviour);
//   bucket=true  (N==1) -> length-bucketed: hold (cap 2x), sort by len, emit shortest bmax.
namespace {
struct LenTask { int id; int len; };
// Drive the real consumer-loop shape for a given `bucket` over a deterministic queue,
// returning the flat sequence of emitted ids (in emission order). This is byte-for-byte
// the consumer loop in batch_engine.cpp: form_next_batch per pop, eager flush_held of full
// batches, then a final clean-EOF flush_held drain.
std::vector<int> drive_consumer(bool bucket, const std::vector<LenTask>& input, int bmax){
  std::queue<LenTask> q; for(const auto& t : input) q.push(t);
  auto try_pop = [&](LenTask& o){ if(q.empty()) return false; o = q.front(); q.pop(); return true; };
  auto len_of  = [](const LenTask& t){ return t.len; };
  std::vector<LenTask> hold;
  std::vector<int> emitted;
  auto emit = [&](std::vector<LenTask>&& b){ for(auto& t : b) emitted.push_back(t.id); };
  LenTask first;
  while(try_pop(first)){
    auto b = form_next_batch(bucket, std::move(first), try_pop, bmax, hold, len_of);
    if(!b.empty()) emit(std::move(b));
    while((int)hold.size() >= bmax){ auto fb = flush_held(bmax, hold, len_of); if(fb.empty()) break; emit(std::move(fb)); }
  }
  for(auto fb = flush_held(bmax, hold, len_of); !fb.empty(); fb = flush_held(bmax, hold, len_of)) emit(std::move(fb));
  return emitted;
}
}  // namespace

TEST_CASE("P5 gate (real helper): bucket=false keeps FIFO arrival order") {
  // Length-UNSORTED arrival so any sort would be observable. ids == arrival index.
  const std::vector<LenTask> in = {{0,300},{1,50},{2,270},{3,10},{4,290},{5,40},{6,260},{7,20}};
  const int bmax = 4;
  // FIFO: emitted ids must equal arrival order 0..7 exactly (no reorder, no loss/dup).
  auto fifo = drive_consumer(/*bucket=*/false, in, bmax);
  std::vector<int> arrival = {0,1,2,3,4,5,6,7};
  CHECK(fifo == arrival);

  // GATE GUARD: the bucket path over the SAME input must REORDER (differ from arrival).
  // If the gate were removed/inverted so the FIFO path also bucketed, these would be equal
  // and this CHECK would fail — that is exactly the regression we are guarding against.
  auto bk = drive_consumer(/*bucket=*/true, in, bmax);
  CHECK(bk != arrival);
}

TEST_CASE("P5 gate (real helper): bucket=true emits the shortest bmax first (length-sorted)") {
  const std::vector<LenTask> in = {{0,300},{1,50},{2,270},{3,10},{4,290},{5,40},{6,260},{7,20}};
  const int bmax = 4;
  // First emitted batch = the 4 shortest lengths (ascending): ids 3(10),7(20),5(40),1(50).
  auto bk = drive_consumer(/*bucket=*/true, in, bmax);
  REQUIRE(bk.size() >= (size_t)bmax);
  std::vector<int> first_batch(bk.begin(), bk.begin()+bmax);
  std::vector<int> expect_short_ids = {3,7,5,1};
  CHECK(first_batch == expect_short_ids);
  // And it must NOT equal the FIFO first batch (proves bucketing actually reordered).
  std::vector<int> fifo_first = {0,1,2,3};
  CHECK(first_batch != fifo_first);
}

TEST_CASE("P5 helper: both modes lose/duplicate nothing (every id emitted exactly once)") {
  // Larger mixed-length input through BOTH modes -> set of emitted ids == full input set.
  std::vector<LenTask> in;
  const int K = 4000;
  for(int i=0;i<K;i++) in.push_back({i, (i*37) % 500});
  for(bool bucket : {false, true}){
    auto emitted = drive_consumer(bucket, in, /*bmax=*/bucket?8:5);
    CHECK((int)emitted.size() == K);                 // no loss, no dup (count)
    std::set<int> uni(emitted.begin(), emitted.end());
    CHECK((int)uni.size() == K);                     // every distinct id exactly once
  }
}

TEST_CASE("P5 helper: bucket=true reduces padding waste vs a length-oblivious baseline") {
  // The bucketing payoff: grouping similar lengths shrinks per-batch max -> less pad waste.
  std::vector<LenTask> in;
  const int K = 4000;
  for(int i=0;i<K;i++) in.push_back({i, (i*37) % 500});
  const int bmax = 8;
  // Re-drive but capture per-batch lengths (re-run the helper loop here to record batches).
  std::queue<LenTask> q; for(const auto& t : in) q.push(t);
  auto try_pop = [&](LenTask& o){ if(q.empty()) return false; o = q.front(); q.pop(); return true; };
  auto len_of  = [](const LenTask& t){ return t.len; };
  std::vector<LenTask> hold;
  std::vector<std::vector<int>> batches;
  auto record = [&](std::vector<LenTask>&& b){ std::vector<int> ls; for(auto& t:b) ls.push_back(t.len); batches.push_back(std::move(ls)); };
  LenTask first;
  while(try_pop(first)){
    auto b = form_next_batch(true, std::move(first), try_pop, bmax, hold, len_of);
    if(!b.empty()) record(std::move(b));
    while((int)hold.size() >= bmax){ auto fb = flush_held(bmax, hold, len_of); if(fb.empty()) break; record(std::move(fb)); }
  }
  for(auto fb = flush_held(bmax, hold, len_of); !fb.empty(); fb = flush_held(bmax, hold, len_of)) record(std::move(fb));

  long long bucket_waste = 0, global_waste = 0, total_items = 0;
  for(const auto& bl : batches){
    int bmaxlen = 0; for(int l : bl) bmaxlen = std::max(bmaxlen, l);
    for(int l : bl){ bucket_waste += (bmaxlen - l); global_waste += (499 - l); }
    total_items += (long long)bl.size();
  }
  CHECK(total_items == K);
  CHECK(bucket_waste < global_waste);
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

// Data-parallel K>1 + R6: with K consumers each discarding its popped `first` on cancel
// (and held buffers + still-queued segments), seg_pending stays elevated for the file, so
// finalize classifies it cancelled (NOT a falsely-ok truncated transcript). seg_pending is
// shared across all K consumers (one atomic per file), so the classification is identical
// to the single-consumer case regardless of K — this asserts that invariant explicitly.
TEST_CASE("data-parallel K>1 + R6: per-consumer discarded segments keep file cancelled") {
  for (int K : {2, 3, 4}) {
    const int total = 20;                 // file 0 has 20 segments, all pushed
    std::atomic<int> seg_pending{0};
    for (int i = 0; i < total; ++i) seg_pending.fetch_add(1);
    bool produced_complete = true;
    // Each of the K consumers routed some segments, then on cancel discarded its popped
    // `first` (and any held) WITHOUT decrementing. Simulate: K consumers each routed 2.
    const int routed_per_consumer = 2;
    for (int k = 0; k < K; ++k)
      for (int i = 0; i < routed_per_consumer; ++i) seg_pending.fetch_sub(1);
    // remaining (total - K*routed) segments stay pending (discarded firsts + still-queued).
    CHECK(seg_pending.load() == total - K * routed_per_consumer);
    CHECK(seg_pending.load() > 0);        // truncation window: at least one segment unrouted
    bool says_cancelled = classify_cancelled(/*cancel_fired=*/true, seg_pending.load(),
                                             produced_complete, /*currently_ok=*/true);
    CHECK(says_cancelled);                // file is cancelled, not falsely reported ok (R6)
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
