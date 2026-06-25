#pragma once
#include "core/types.h"
#include "core/config.h"
#include "core/hardware.h"
#include <string>
#include <vector>
#include <functional>
namespace suji {
struct FileResult { std::string input; bool ok=false; std::string err; Transcript transcript; };
struct BatchProgress { int files_total=0; int files_done=0; double audio_seconds_done=0; };
using ProgressCb = std::function<void(const BatchProgress&)>;
// Decodes+VADs files on producer threads, batches ASR on one consumer (owns recognizer),
// then per-file: sort tokens by time -> merge_tokens -> punctuate -> Transcript.
std::vector<FileResult> transcribe_batch_files(const std::vector<std::string>& inputs,
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb = nullptr);
}
