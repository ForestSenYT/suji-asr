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
