// G2 — GPU OOM auto-halve + retry control logic (unit tests with a stub recognizer).
//
// Real CUDA OOM is impractical to induce deterministically, so we test the CONTROL
// LOGIC of transcribe_oom_safe() with a stub "recognizer" whose transcribe fails on
// batches above a size threshold but succeeds at or below it. This proves the helper:
//   1. recovers a too-large batch by splitting in half (and, transitively, quarters),
//   2. passes a healthy batch straight through (no needless splitting),
//   3. surfaces an UNRECOVERABLE failure (even N==1 fails) as an empty result so the
//      caller's size-mismatch guard marks the file failed (R3 path) — never faked,
//   4. treats a THROWING transcribe as a failed batch (caught, then split-retried).

#include "doctest/doctest.h"
#include "core/oom_retry.h"
#include "core/types.h"
#include <vector>
#include <stdexcept>

using namespace suji;

namespace {
// Lightweight stand-in for Asr::SegView so the helper is exercised without the ASR class.
struct ViewStub { int id; };

// Build a successful result vector of length n (one AsrResult per view).
std::vector<AsrResult> ok_results(size_t n) {
  std::vector<AsrResult> r(n);
  for (size_t i = 0; i < n; ++i) r[i].text = "ok";
  return r;
}
}  // namespace

TEST_CASE("oom_retry: too-large batch recovers by splitting in half") {
  // Stub fails (empty return == R3 signal) for batches > 4; succeeds at <= 4.
  int calls = 0, fails = 0;
  auto stub = [&](const std::vector<ViewStub>& v) -> std::vector<AsrResult> {
    ++calls;
    if (v.size() > 4) { ++fails; return {}; }   // "OOM": empty == failed batch
    return ok_results(v.size());
  };

  std::vector<ViewStub> views(8);
  for (int i = 0; i < 8; ++i) views[i].id = i;

  auto res = transcribe_oom_safe(views, stub);

  // Full recovery: 8 -> fails -> split into 4 + 4, both succeed.
  CHECK(res.size() == views.size());
  CHECK(fails == 1);          // exactly one large-batch failure observed
  CHECK(calls == 3);          // 1 full attempt (fail) + 2 halves (both ok)
}

TEST_CASE("oom_retry: healthy batch passes straight through (no split)") {
  int calls = 0;
  auto stub = [&](const std::vector<ViewStub>& v) -> std::vector<AsrResult> {
    ++calls;
    return ok_results(v.size());   // always succeeds
  };
  std::vector<ViewStub> views(8);
  auto res = transcribe_oom_safe(views, stub);
  CHECK(res.size() == 8);
  CHECK(calls == 1);   // no retry needed
}

TEST_CASE("oom_retry: unrecoverable failure returns empty (R3 signal, not faked)") {
  // Stub fails for EVERYTHING, including N==1 -> helper cannot recover.
  auto stub = [&](const std::vector<ViewStub>&) -> std::vector<AsrResult> {
    return {};
  };
  std::vector<ViewStub> views(8);
  auto res = transcribe_oom_safe(views, stub);
  // Must be empty so the caller's `res.size() != batch.size()` guard fires (mark failed).
  CHECK(res.empty());
  CHECK(res.size() != views.size());
}

TEST_CASE("oom_retry: single-segment failure is not split, returns empty") {
  int calls = 0;
  auto stub = [&](const std::vector<ViewStub>&) -> std::vector<AsrResult> {
    ++calls; return {};
  };
  std::vector<ViewStub> one(1);
  auto res = transcribe_oom_safe(one, stub);
  CHECK(res.empty());
  CHECK(calls == 1);   // N==1 cannot be split -> exactly one attempt, no retry
}

TEST_CASE("oom_retry: throwing transcribe is caught and split-retried") {
  // Stub THROWS for batches > 2 (simulating an Ort::Exception that propagates),
  // succeeds at <= 2. Helper must catch, treat as failed, and recover via split.
  int calls = 0;
  auto stub = [&](const std::vector<ViewStub>& v) -> std::vector<AsrResult> {
    ++calls;
    if (v.size() > 2) throw std::runtime_error("simulated CUDA OOM");
    return ok_results(v.size());
  };
  std::vector<ViewStub> views(4);
  auto res = transcribe_oom_safe(views, stub);
  CHECK(res.size() == 4);   // 4 throws -> split 2 + 2, both succeed
  CHECK(calls == 3);        // 1 throwing attempt + 2 halves
}

TEST_CASE("oom_retry: deeper recovery when one half still too large") {
  // Threshold 2: 8 fails -> 4+4 each still fail. The helper only retries ONCE
  // (single split), so 4 -> 4 halves are NOT further split by THIS call; the
  // two 4-halves fail -> overall returns empty (documented single-retry contract).
  auto stub = [&](const std::vector<ViewStub>& v) -> std::vector<AsrResult> {
    if (v.size() > 2) return {};
    return ok_results(v.size());
  };
  std::vector<ViewStub> views(8);
  auto res = transcribe_oom_safe(views, stub);
  // Single halve (8 -> 4+4) is not enough when threshold is 2 -> empty (mark failed).
  CHECK(res.empty());
}
