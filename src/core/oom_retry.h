#pragma once
#include "core/types.h"
#include "core/log.h"
#include <vector>
#include <string>

namespace suji {

// G2 — GPU OOM auto-halve + retry control logic.
//
// Templated on the view container element type (View) and a transcribe functor (Fn)
// so it is unit-testable with a stub recognizer WITHOUT a real GPU. The production
// caller (batch_engine.cpp) instantiates View = Asr::SegView and Fn = a lambda over
// Asr::transcribe_batch.
//
// OOM catchability (investigated against asr.cpp + the sherpa-onnx C ABI):
//   * The reliable, ALWAYS-detectable OOM signal is stream-creation returning NULL,
//     which Asr::transcribe_batch already converts into an EMPTY result vector
//     (size != views.size()). That is the R3 failure signal this helper keys off.
//   * A CUDA OOM raised DEEPER (during DecodeMultipleOfflineStreams) surfaces inside
//     the sherpa-onnx DLL as an Ort::Exception. Whether it unwinds across the
//     extern "C" boundary back to us is build-dependent (MSVC may propagate it, or
//     ORT may std::terminate/abort). We ALSO wrap the call in try/catch: if the
//     exception DOES propagate it is caught and treated as a failed batch; if ORT
//     hard-aborts instead, no C++ code can recover and halve-retry cannot help
//     (documented, not faked).
//
// Strategy: run the functor on the views. On failure (empty / wrong-size return, OR a
// caught exception) with N>1 views, retry ONCE by splitting in half and transcribing
// each half independently; concatenate only if BOTH halves succeed. If a half (or an
// N==1 batch) still fails, return {} so the caller's size-mismatch guard fires and the
// file is marked failed (existing R3 path). Output is never fabricated.
template <class View, class Fn>
std::vector<AsrResult> transcribe_oom_safe(const std::vector<View>& views, Fn&& transcribe) {
  auto run = [&](const std::vector<View>& v) -> std::vector<AsrResult> {
    try {
      auto r = transcribe(v);
      if (r.size() == v.size()) return r;   // success
      return {};                            // wrong-size == failed batch (R3 signal)
    } catch (...) {
      return {};                            // caught Ort/CUDA exception -> failed batch
    }
  };
  auto res = run(views);
  if (!res.empty() || views.size() <= 1) return res;   // ok, or can't split further

  // Failure on a multi-segment batch: split in half and retry ONCE.
  log_err("transcribe_batch failed on " + std::to_string(views.size()) +
          "-segment batch (suspected GPU OOM); retrying split in half");
  size_t mid = views.size() / 2;
  std::vector<View> lo(views.begin(), views.begin() + mid);
  std::vector<View> hi(views.begin() + mid, views.end());
  auto rlo = run(lo);
  auto rhi = run(hi);
  if (rlo.size() == lo.size() && rhi.size() == hi.size()) {
    rlo.insert(rlo.end(), rhi.begin(), rhi.end());   // both halves recovered
    return rlo;
  }
  return {};   // halved retry also failed -> caller marks the files failed (R3)
}

}  // namespace suji
