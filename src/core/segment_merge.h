#pragma once
#include "core/types.h"
#include <vector>

namespace suji {
std::vector<Segment> merge_tokens(const std::vector<Token>& toks, double gap_sec, double max_dur_sec);
} // namespace suji
