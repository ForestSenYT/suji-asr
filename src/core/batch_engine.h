#pragma once
#include "core/types.h"
#include "core/config.h"
#include "core/hardware.h"
#include "core/cancel.h"
#include <string>
#include <vector>
#include <functional>
namespace suji {
struct FileResult { std::string input; bool ok=false; std::string err; Transcript transcript; };
// audio_seconds_done = VAD-SPEECH seconds consumed so far (drives the live % bar).
// total_audio_decoded = full DECODED file duration (incl. silence) accumulated across
//   producers; used for the FINAL aggregate throughput so it reflects true audio-hours,
//   not just speech. (G13)
// cpu_segs / gpu_segs = LIVE per-consumer segment counts (hetero engine only; 0 for
//   single-provider paths). Lets the GUI show a live "CPU% / GPU%" split instead of
//   only logging the ratio after completion. (G14)
struct BatchProgress {
  int files_total=0; int files_done=0;
  double audio_seconds_done=0; double total_audio_decoded=0;
  long long cpu_segs=0; long long gpu_segs=0;
};
using ProgressCb = std::function<void(const BatchProgress&)>;
// Decodes+VADs files on producer threads, batches ASR on one consumer (owns recognizer),
// then per-file: sort tokens by time -> merge_tokens -> punctuate -> Transcript.
// cancel == nullptr: original behaviour (no cancellation).
std::vector<FileResult> transcribe_batch_files(const std::vector<std::string>& inputs,
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb = nullptr,
    CancelToken* cancel = nullptr);
}
