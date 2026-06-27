#pragma once
#include "core/types.h"
#include <vector>

namespace suji {
std::vector<Segment> merge_tokens(const std::vector<Token>& toks, double gap_sec, double max_dur_sec);

// Per-file finalize: order a file's tokens by time, then merge into segments.
// Uses a STABLE sort because fp16-AED emits no per-token timestamps — every token in a
// segment shares the segment's base time, and an unstable sort would scramble those
// equal-key tokens (jumbling the text). Stable sort keeps the model's emission order
// within a segment while still ordering segments (distinct bases) by time, so it is
// correct regardless of the order consumers processed segments in (hetero / P5 bucketing).
std::vector<Segment> merge_file_tokens(std::vector<Token> toks, double gap_sec, double max_dur_sec);
} // namespace suji
