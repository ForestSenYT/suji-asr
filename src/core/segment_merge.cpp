#include "core/segment_merge.h"

namespace suji {

// T6: constant added to the last token's start when no following token exists.
// 300 ms is a safe minimum for a CJK syllable — far better than the old +2.0s.
static constexpr double kLastTokenPad = 0.3;

std::vector<Segment> merge_tokens(const std::vector<Token>& toks, double gap_sec, double max_dur_sec) {
  std::vector<Segment> segs;
  if (toks.empty()) return segs;

  Segment cur;
  cur.start = toks[0].start;
  cur.end   = toks[0].start;   // will be overwritten when segment closes
  cur.tokens.push_back(toks[0]);
  cur.text = toks[0].text;

  for (size_t i = 1; i < toks.size(); ++i) {
    double gap = toks[i].start - toks[i-1].start;
    double dur = toks[i].start - cur.start;

    if (gap > gap_sec || dur >= max_dur_sec) {
      // T6: segment ends at the START of the next token (the gap's right edge),
      // not the start of the last token within the segment.
      cur.end = toks[i].start;
      segs.push_back(cur);
      cur = Segment{};
      cur.start = toks[i].start;
    }

    cur.tokens.push_back(toks[i]);
    cur.text += toks[i].text;
    cur.end = toks[i].start;   // updated as we advance; overwritten at segment close
  }

  // T6: final segment — no following token; use small constant pad
  cur.end = toks.back().start + kLastTokenPad;
  segs.push_back(cur);
  return segs;
}

} // namespace suji
