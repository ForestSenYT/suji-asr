#include "core/segment_merge.h"

namespace suji {

// T6: constant added to the last token's start when no following token exists.
// 300 ms is a safe minimum for a CJK syllable — far better than the old +2.0s.
static constexpr double kLastTokenPad = 0.3;

// int8-CTC leaks silence/special markers (e.g. "< sil >") into the token stream.
// These must never appear in the visible transcript. A token is a special marker
// (and is dropped) when its WHITESPACE-TRIMMED text is empty, or is fully wrapped
// in angle brackets (^<.*>$) — this catches "< sil >", "<sil>", "<blank>", "<unk>",
// "<s>", etc. Real content tokens that merely *contain* a stray '<' or '>' (e.g.
// "a<b") are kept. fp16-AED doesn't emit these markers, so it's unaffected.
static bool is_special_token(const std::string& t) {
  size_t b = t.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return true;            // empty / all-whitespace
  size_t e = t.find_last_not_of(" \t\r\n");
  return t[b] == '<' && t[e] == '>';                  // fully angle-bracketed
}

std::vector<Segment> merge_tokens(const std::vector<Token>& raw, double gap_sec, double max_dur_sec) {
  std::vector<Segment> segs;

  // Pre-filter special/silence markers, keeping real content tokens (and their
  // timestamps) intact, then run the existing merge on the clean stream so all
  // T6 start/end/gap semantics are computed only from real tokens.
  std::vector<Token> toks;
  toks.reserve(raw.size());
  for (const auto& t : raw)
    if (!is_special_token(t.text)) toks.push_back(t);

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
