#include "core/segment_merge.h"

namespace suji {
std::vector<Segment> merge_tokens(const std::vector<Token>& toks, double gap_sec, double max_dur_sec) {
  std::vector<Segment> segs;
  if (toks.empty()) return segs;

  Segment cur;
  cur.start = toks[0].start;
  cur.end = toks[0].start;
  cur.tokens.push_back(toks[0]);
  cur.text = toks[0].text;

  for (size_t i = 1; i < toks.size(); ++i) {
    double gap = toks[i].start - toks[i-1].start;
    double dur = toks[i].start - cur.start;

    if (gap > gap_sec || dur >= max_dur_sec) {
      segs.push_back(cur);
      cur = Segment{};
      cur.start = toks[i].start;
    }

    cur.tokens.push_back(toks[i]);
    cur.text += toks[i].text;
    cur.end = toks[i].start;
  }

  segs.push_back(cur);
  return segs;
}
} // namespace suji
