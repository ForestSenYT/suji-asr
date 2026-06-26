#pragma once
#include <vector>
#include <algorithm>
#include <iterator>
namespace suji {
// P5 gate (real). Form the next ASR batch from a just-popped `first` task plus a
// `try_pop` source, gated by `bucket`:
//
//   bucket == false  (N>1, multi-file): the EXACT pre-P5 FIFO fast path. Take `first`,
//     try_pop up to bmax-1 MORE in ARRIVAL ORDER, return that one batch. No `hold`, no
//     sort. Byte-for-byte the pre-P5 multi-file behaviour (restores work-stealing balance).
//
//   bucket == true   (N==1, single-file): length-bucketed reorder window. Push `first`
//     into `hold`, drain via try_pop up to cap (2x bmax) so the held buffer can be sorted
//     by sample length; once we hold >= bmax, sort and return the shortest contiguous
//     `bmax` (the most length-similar run -> less padding waste). Remainder stays in `hold`
//     for the next call / the EOF flush.
//
// `len_of` extracts the length key used for bucketing (sample count). `try_pop(out)`
// returns true and fills `out` when an item was available, false otherwise.
//
// Returns the batch to process. In bucket mode it may be empty (held < bmax and the
// queue is momentarily drained) — the caller simply loops/continues; the held remainder
// is flushed by flush_held() on a clean EOF.
template<class T, class TryPop, class LenOf>
std::vector<T> form_next_batch(bool bucket, T&& first, TryPop&& try_pop,
                               int bmax, std::vector<T>& hold, LenOf&& len_of) {
  if (bmax < 1) bmax = 1;
  if (!bucket) {
    // FIFO: one batch, arrival order, no hold/sort.
    std::vector<T> batch;
    batch.push_back(std::move(first));
    T more;
    while ((int)batch.size() < bmax && try_pop(more)) batch.push_back(std::move(more));
    return batch;
  }
  // Bucket: accumulate into hold (cap 2x bmax), then emit the shortest bmax if we have them.
  const size_t cap = (size_t)bmax * 2;
  hold.push_back(std::move(first));
  T more;
  while (hold.size() < cap && try_pop(more)) hold.push_back(std::move(more));
  if ((int)hold.size() < bmax) return {};   // not enough yet; keep holding
  std::sort(hold.begin(), hold.end(),
            [&](const T& a, const T& b){ return len_of(a) < len_of(b); });
  size_t take = std::min((size_t)bmax, hold.size());
  std::vector<T> batch(std::make_move_iterator(hold.begin()),
                       std::make_move_iterator(hold.begin() + take));
  hold.erase(hold.begin(), hold.begin() + take);
  return batch;
}

// EOF flush for the bucket path: sort the held remainder by length and pop the next
// (up to) bmax as one batch. Returns empty when `hold` is empty. The FIFO path never
// holds anything, so this is a no-op there.
template<class T, class LenOf>
std::vector<T> flush_held(int bmax, std::vector<T>& hold, LenOf&& len_of) {
  if (bmax < 1) bmax = 1;
  if (hold.empty()) return {};
  std::sort(hold.begin(), hold.end(),
            [&](const T& a, const T& b){ return len_of(a) < len_of(b); });
  size_t take = std::min((size_t)bmax, hold.size());
  std::vector<T> batch(std::make_move_iterator(hold.begin()),
                       std::make_move_iterator(hold.begin() + take));
  hold.erase(hold.begin(), hold.begin() + take);
  return batch;
}
}  // namespace suji
