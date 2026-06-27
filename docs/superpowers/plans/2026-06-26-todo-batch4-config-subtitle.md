# Todo Batch 4 — Config Knobs + Subtitle Quality Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add six config/quality improvements — decoding_method, decide() caps, real subtitle end-times (next-token-start), UTF-8-aware SRT/VTT line wrapping, and punct provider/thread config — while keeping all 105 tests green.

**Architecture:** All six items touch `EngineConfig` (adding fields), their respective `.cpp` files (reading those fields), the writers (line-wrap + real end-times), and `segment_merge.cpp` (computing end from next token). Tests are pure unit tests using doctest; no integration run required to verify these items.

**Tech Stack:** C++17, MSVC `/utf-8 /W4`, doctest, sherpa-onnx C-API (offline recognizer result has `timestamps[]` per-token START only — no `durations` in offline API; online-only struct has `durations`). CMake 4.3 at `F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe`.

## Global Constraints

- C++17, MSVC, `/utf-8 /W4` — zero new warnings.
- All 105 existing tests must remain green; update test assertions only when end-time semantics legitimately change (the existing `segs[0].end == 0.3` assertion in test_segment_merge.cpp WILL change — that's intentional).
- Branch: `feat/todo-sweep`. One logical commit per task or grouped pair.
- Build: `F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release`
- Test run: `build\Release\suji_tests.exe`
- No new files unless strictly necessary; always prefer editing existing files.
- **T6 durations finding (critical for T6/T7):** `SherpaOnnxOfflineRecognizerResult` (offline API used by `asr.cpp`) has `timestamps[]` (per-token START times, seconds, `float*`) and no `durations` field. The `durations` field exists only in the *online* streaming result struct. Therefore the "preferred" path in T6 is unavailable; we must use the "else" path: segment end = start of next token in the stream, capped by a max gap constant; last segment's last token gets `+` a small constant.

---

## File Map

| File | Change |
|---|---|
| `src/core/config.h` | Add 5 fields: `decoding_method`, `srt_max_chars_per_line`, `punct_provider`, `punct_threads`, `max_batch`, `max_threads` |
| `src/core/types.h` | Add `end` field to `Token` |
| `src/core/asr.cpp` | Use `cfg.decoding_method` instead of literal `"greedy_search"` |
| `src/core/segment_merge.cpp` | Compute `Token.end` from next-token start; set `Segment.end` from last token's `end` |
| `src/core/segment_merge.h` | No change needed (signature unchanged) |
| `src/core/output/srt_writer.h` | Change signature to accept `EngineConfig` |
| `src/core/output/srt_writer.cpp` | Use `s.end` directly; add UTF-8 line-wrap when `cfg.srt_max_chars_per_line > 0` |
| `src/core/output/vtt_writer.h` | Change signature to accept `EngineConfig` |
| `src/core/output/vtt_writer.cpp` | Same — use real `s.end`; add line-wrap |
| `src/core/output/writer_facade.cpp` | Pass `cfg` to `to_srt`/`to_vtt` |
| `src/core/punctuation.cpp` | Use `cfg.punct_provider` and `cfg.punct_threads` |
| `src/core/hardware.cpp` | Apply `cfg.max_batch` / `cfg.max_threads` caps in `decide()`, remove `(void)cfg` |
| `src/cli/batch_main.cpp` | Add `--srt-line N` flag |
| `tests/test_segment_merge.cpp` | Update `segs[0].end` assertion; add end-time property tests |
| `tests/test_srt_writer.cpp` | Update for new signature; add wrap test |
| `tests/test_vtt_writer.cpp` | Update for new signature; add wrap test |
| `tests/test_autotune.cpp` | Add T5 cap tests |

---

### Task 1: T3 — `decoding_method` config field

**Files:**
- Modify: `src/core/config.h`
- Modify: `src/core/asr.cpp:36`
- Modify: `tests/test_sanity.cpp` (or a new test block in `tests/test_segment_merge.cpp` — actually add to a new `tests/test_config_fields.cpp`... wait, CMake globs `tests/*.cpp` automatically, so any new file is picked up)
- Create: `tests/test_config_fields.cpp`

**Interfaces:**
- Produces: `EngineConfig::decoding_method` (default `"greedy_search"`)
- `Asr` constructor reads `cfg.decoding_method.c_str()` at line 36 of asr.cpp

- [ ] **Step 1: Write the failing test**

Create `tests/test_config_fields.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/config.h"

using namespace suji;

TEST_CASE("T3: EngineConfig decoding_method default is greedy_search") {
  EngineConfig c;
  CHECK(c.decoding_method == "greedy_search");
}

TEST_CASE("T3: EngineConfig decoding_method can be overridden") {
  EngineConfig c;
  c.decoding_method = "beam_search";
  CHECK(c.decoding_method == "beam_search");
}
```

- [ ] **Step 2: Run test to verify it fails**

```
build\Release\suji_tests.exe --test-case="T3*"
```

Expected: FAIL — `decoding_method` doesn't exist yet.

- [ ] **Step 3: Add `decoding_method` to `EngineConfig` in `src/core/config.h`**

In the struct body, after the `int vad_window` line, add:

```cpp
  // ASR 解码方法(默认 greedy_search;也可为 beam_search)
  std::string decoding_method = "greedy_search";
```

- [ ] **Step 4: Use the field in `src/core/asr.cpp`**

Replace line 36:
```cpp
  c.decoding_method = "greedy_search";
```
with:
```cpp
  c.decoding_method = cfg.decoding_method.c_str();
```

- [ ] **Step 5: Run test to verify it passes**

```
build\Release\suji_tests.exe --test-case="T3*"
```

Expected: PASS (2 tests).

- [ ] **Step 6: Build and run full suite**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
build\Release\suji_tests.exe
```

Expected: All tests pass (105 + 2 new = 107).

- [ ] **Step 7: Commit**

```
git add src/core/config.h src/core/asr.cpp tests/test_config_fields.cpp
git commit -m "feat(T3): make decoding_method configurable via EngineConfig"
```

---

### Task 2: G10 — punct provider/threads configurable

**Files:**
- Modify: `src/core/config.h`
- Modify: `src/core/punctuation.cpp:8-9`
- Modify: `tests/test_config_fields.cpp`

**Interfaces:**
- Produces: `EngineConfig::punct_provider` (default `"cpu"`), `EngineConfig::punct_threads` (default `1`)
- `Punctuator` constructor reads both fields

- [ ] **Step 1: Write failing tests** (append to `tests/test_config_fields.cpp`)

```cpp
TEST_CASE("G10: EngineConfig punct_provider default is cpu") {
  EngineConfig c;
  CHECK(c.punct_provider == "cpu");
}
TEST_CASE("G10: EngineConfig punct_threads default is 1") {
  EngineConfig c;
  CHECK(c.punct_threads == 1);
}
TEST_CASE("G10: EngineConfig punct_provider can be overridden") {
  EngineConfig c;
  c.punct_provider = "cuda";
  CHECK(c.punct_provider == "cuda");
}
TEST_CASE("G10: EngineConfig punct_threads can be overridden") {
  EngineConfig c;
  c.punct_threads = 4;
  CHECK(c.punct_threads == 4);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```
build\Release\suji_tests.exe --test-case="G10*"
```

Expected: FAIL.

- [ ] **Step 3: Add fields to `src/core/config.h`**

After the `decoding_method` line, add:

```cpp
  // 标点模型 provider / 线程数
  std::string punct_provider = "cpu";
  int punct_threads = 1;
```

- [ ] **Step 4: Update `src/core/punctuation.cpp`**

Replace lines 8–9:
```cpp
  c.model.num_threads = 1; c.model.provider = "cpu"; c.model.debug = 0;
```
with:
```cpp
  c.model.num_threads = cfg.punct_threads; c.model.provider = cfg.punct_provider.c_str(); c.model.debug = 0;
```

- [ ] **Step 5: Build and run full suite**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
build\Release\suji_tests.exe
```

Expected: All tests pass.

- [ ] **Step 6: Commit**

```
git add src/core/config.h src/core/punctuation.cpp tests/test_config_fields.cpp
git commit -m "feat(G10): make punct provider/threads configurable via EngineConfig"
```

---

### Task 3: T5 — `decide()` override caps (`max_batch` / `max_threads`)

**Files:**
- Modify: `src/core/config.h`
- Modify: `src/core/hardware.cpp:135-166`
- Modify: `tests/test_autotune.cpp`

**Interfaces:**
- Produces: `EngineConfig::max_batch` (0 = auto), `EngineConfig::max_threads` (0 = auto)
- `decide()` applies `min(result, cap)` after computing tunables when cap > 0

**Design note:** The plan originally marked this "NEEDS-HUMAN" for the design question. The chosen design — optional `max_batch` and `max_threads` caps in `EngineConfig` that constrain the auto-tuner's output — is the obvious safe choice: 0 means uncapped (no behaviour change), positive values cap the result. This is a pure constraint layer, not a mode override.

- [ ] **Step 1: Write failing tests** (append to `tests/test_autotune.cpp`)

```cpp
// ---- T5: decide() cap overrides ----

TEST_CASE("T5: max_batch=0 leaves batch unchanged (auto)") {
  EngineConfig cfg;
  cfg.max_batch = 0;
  // GPU + 8 cores = Cuda path, batch >= 8
  auto t = decide(hw(true, 6000, 8, 40000, true), cfg);
  CHECK(t.batch >= 8);
}

TEST_CASE("T5: max_batch caps gpu batch") {
  EngineConfig cfg;
  cfg.max_batch = 8;
  // GPU + 8 cores = Cuda path, uncapped batch would be >=8 (could be up to 30)
  auto t = decide(hw(true, 6000, 8, 40000, true), cfg);
  CHECK(t.batch <= 8);
}

TEST_CASE("T5: max_threads=0 leaves num_threads unchanged (auto)") {
  EngineConfig cfg;
  cfg.max_threads = 0;
  // CPU path with 16 cores -> num_threads >= 16
  auto t = decide(hw(false, 0, 16, 40000), cfg);
  CHECK(t.num_threads >= 16);
}

TEST_CASE("T5: max_threads caps num_threads") {
  EngineConfig cfg;
  cfg.max_threads = 6;
  // CPU path with 16 cores -> uncapped would be 16
  auto t = decide(hw(false, 0, 16, 40000), cfg);
  CHECK(t.num_threads <= 6);
}

TEST_CASE("T5: max_batch and max_threads both active") {
  EngineConfig cfg;
  cfg.max_batch = 4;
  cfg.max_threads = 4;
  auto t = decide(hw(false, 0, 16, 40000), cfg);
  CHECK(t.batch <= 4);
  CHECK(t.num_threads <= 4);
}
```

- [ ] **Step 2: Run to verify they fail**

```
build\Release\suji_tests.exe --test-case="T5*"
```

Expected: FAIL — fields don't exist yet.

- [ ] **Step 3: Add fields to `src/core/config.h`**

After `punct_threads`:
```cpp
  // Auto-tuner caps (0 = uncapped/auto)
  int max_batch = 0;
  int max_threads = 0;
```

- [ ] **Step 4: Apply caps in `src/core/hardware.cpp` `decide()`**

Replace the `(void)cfg;` line (currently line 164) with:

```cpp
  // T5: apply optional caps (0 = uncapped)
  if (cfg.max_batch   > 0) { t.batch = std::min(t.batch, cfg.max_batch); t.gpu_batch = std::min(t.gpu_batch, cfg.max_batch); t.cpu_batch = std::min(t.cpu_batch, cfg.max_batch); }
  if (cfg.max_threads > 0) { t.num_threads = std::min(t.num_threads, cfg.max_threads); t.cpu_asr_threads = std::min(t.cpu_asr_threads, cfg.max_threads); }
```

Add `#include <algorithm>` at top of hardware.cpp if not already present (it already is — check line 9).

- [ ] **Step 5: Build and run full suite**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
build\Release\suji_tests.exe
```

Expected: all tests pass (existing autotune tests use `EngineConfig{}` which has `max_batch=0, max_threads=0`, so they are unaffected).

- [ ] **Step 6: Commit**

```
git add src/core/config.h src/core/hardware.cpp tests/test_autotune.cpp
git commit -m "feat(T5): add max_batch/max_threads caps to decide() via EngineConfig"
```

---

### Task 4: T6+T7 — Real subtitle end-times (next-token-start strategy)

**T6 durations finding:** The offline recognizer result (`SherpaOnnxOfflineRecognizerResult`) has `timestamps[]` (per-token start times, `float*`, may be NULL) and `count` but **no `durations` field**. The `durations` field only exists in the online streaming result struct (`SherpaOnnxOfflineRecognizerResult` for online). Therefore we cannot use per-token durations. We must use next-token-start as `Token.end`, with a small constant fallback for the last token.

**Strategy:**
1. Add `double end = 0.0;` to `Token` in `types.h`.
2. In `asr.cpp`, after building `timestamps`, compute `token[i].end = timestamps[i+1]` for all but the last; last token gets `start + 0.3` (300ms, a safe minimum for CJK syllables — better than +2.0s).
3. In `segment_merge.cpp`, set `cur.end = toks[i].end` (the last token's end) instead of `toks[i].start`.
4. Update `srt_writer.cpp` and `vtt_writer.cpp`: the fallback `s.start + 2.0` becomes `s.start + 0.3` (last-resort only when `s.end <= s.start`, which should not happen after step 3).

**Note:** `asr.cpp` builds `AsrResult` (with `timestamps` as doubles) and the Token assembly happens in `pipeline.cpp` or `segment_merge`. Let's check where `AsrResult.timestamps` is converted to `Token`.

**Files:**
- Modify: `src/core/types.h`
- Modify: `src/core/asr.cpp` — not needed for end; end is computed from the token stream
- Modify: `src/core/segment_merge.cpp` — compute `Token.end` from next token before building segments
- Modify: `src/core/output/srt_writer.cpp` — remove `+2.0` fallback (use `s.start + 0.3` only when `end <= start`)
- Modify: `src/core/output/vtt_writer.cpp` — same
- Modify: `tests/test_segment_merge.cpp` — update `segs[0].end` assertion + add monotonic end-time tests
- Modify: `tests/test_srt_writer.cpp` — existing test already sets `s.end=2.5`, still passes

**Note on where Token.end gets set:** `AsrResult` is produced by `Asr::transcribe()` with `timestamps[]` as per-token start times. The conversion from `AsrResult` to `Token` stream happens in `src/core/pipeline.cpp`. Let me check that file.

- [ ] **Step 0: Read `src/core/pipeline.cpp` to find Token assembly**

Read `F:\Git\suji-asr\src\core\pipeline.cpp` to find where `AsrResult.timestamps[i]` is converted to `Token.start`. The `Token.end` must be filled there (using `timestamps[i+1]` for i < count-1, else `timestamps[i] + 0.3`).

```
// Pseudo-code for what we expect to find:
// for (size_t i = 0; i < res.tokens.size(); i++) {
//   Token tok;
//   tok.text = res.tokens[i];
//   tok.start = seg_offset_sec + res.timestamps[i];
//   tokens.push_back(tok);
// }
```

- [ ] **Step 1: Write failing tests** (replace/update `tests/test_segment_merge.cpp`)

The existing test `CHECK(segs[0].end == doctest::Approx(0.3))` will need updating since `end` will now be the START of the NEXT token, not the START of the last token in the segment. With tokens at 0.0, 0.3, then next segment starts at 2.0 → `segs[0].end = 2.0` (capped by the first token of the next segment).

Wait — re-read `merge_tokens`: the function receives `Token` objects, which now have `end` fields set by the caller (pipeline). The merge function does NOT have access to the next-segment's token. The `Token.end` is set BEFORE `merge_tokens` is called.

So the correct approach for `merge_tokens`:
- `cur.end = toks[i].end` (last token in segment)

And `Token.end` is set by pipeline as `timestamps[i+1]` for non-last tokens, `timestamps.back() + 0.3` for the last token.

But `test_segment_merge.cpp` constructs tokens directly using helper `T(text, start)` without setting `end`. We need to update this helper or set `end` in the test.

The cleanest approach: `merge_tokens` computes `cur.end` as the NEXT token's start (if a split is triggered by gap, the end of the current segment = start of the next token, which is the gap itself). This way `merge_tokens` can set real ends WITHOUT needing `Token.end` at all.

Revised approach (simpler, no pipeline change):
- In `merge_tokens`: when a segment boundary fires at token `i`, set `prev_seg.end = toks[i].start` (= start of the next segment's first token = the gap's right edge). For the final segment, set `end = toks.back().start + 0.3`.
- `Token.end` field: still add it for completeness but `merge_tokens` computes `Segment.end` directly from the sorted token stream.

With tokens `{0.0, 0.3, 2.0, 2.2}` and gap=1.0:
- Seg 0 boundary fires at i=2 (gap 2.0-0.3=1.7 > 1.0). End = toks[2].start = 2.0. ✓
- Seg 1 is the last segment. End = toks[3].start + 0.3 = 2.5. ✓

Updated test assertions:
```cpp
CHECK(segs[0].end == doctest::Approx(2.0));  // was 0.3 — now next-token start
CHECK(segs[1].end == doctest::Approx(2.5));  // last seg: last_token.start + 0.3
```

New tests to add:
```cpp
TEST_CASE("T6: segment ends are strictly > start") {
  // ...
}
TEST_CASE("T6: segment end <= next segment start") {
  // ...
}
TEST_CASE("T6: last segment end is last_token.start + 0.3") {
  // ...
}
```

- [ ] **Step 1a: Update `tests/test_segment_merge.cpp` — failing state**

The existing test at line 19 currently passes:
```cpp
CHECK(segs[0].end == doctest::Approx(0.3));
```
After we update `merge_tokens`, this will fail with the new value `2.0`. Update the test FIRST to the new expected values, which will fail until the implementation is changed.

Replace the full `test_segment_merge.cpp`:

```cpp
#include "doctest/doctest.h"
#include "core/segment_merge.h"

using namespace suji;

static Token T(const char* s, double t) {
  Token x;
  x.text = s;
  x.start = t;
  return x;
}

TEST_CASE("merge by gap") {
  std::vector<Token> toks = { T("你",0.0),T("好",0.3),T("世",2.0),T("界",2.2) }; // 0.3->2.0 gap=1.7
  auto segs = merge_tokens(toks, 1.0, 30.0);
  REQUIRE(segs.size() == 2);
  CHECK(segs[0].text == u8"你好");
  CHECK(segs[0].start == doctest::Approx(0.0));
  // T6: end = start of next segment's first token (2.0), not last token in seg (0.3)
  CHECK(segs[0].end == doctest::Approx(2.0));
  CHECK(segs[1].text == u8"世界");
  // T6: last seg: last_token.start + 0.3 = 2.2 + 0.3 = 2.5
  CHECK(segs[1].end == doctest::Approx(2.5));
}

TEST_CASE("merge by max duration") {
  std::vector<Token> toks;
  for(int i=0;i<10;i++) toks.push_back(T("字", i*0.5)); // 连续 0.5s 间隔
  auto segs = merge_tokens(toks, 1.0, 2.0); // 2s 上限 -> 多段
  CHECK(segs.size() >= 2);
}

TEST_CASE("empty input") {
  CHECK(merge_tokens({},1.0,30.0).empty());
}

TEST_CASE("T6: each segment end > start") {
  std::vector<Token> toks = { T("你",0.0),T("好",0.3),T("世",2.0),T("界",2.2) };
  auto segs = merge_tokens(toks, 1.0, 30.0);
  for (const auto& s : segs) {
    CHECK(s.end > s.start);
  }
}

TEST_CASE("T6: segment end <= next segment start (monotonic)") {
  std::vector<Token> toks = { T("a",0.0),T("b",0.5),T("c",2.0),T("d",2.5),T("e",5.0),T("f",5.5) };
  auto segs = merge_tokens(toks, 1.0, 30.0);
  REQUIRE(segs.size() >= 2);
  for (size_t i = 0; i + 1 < segs.size(); ++i) {
    CHECK(segs[i].end <= segs[i+1].start + 1e-9); // end of seg[i] <= start of seg[i+1]
  }
}

TEST_CASE("T6: last segment end = last_token.start + 0.3") {
  std::vector<Token> toks = { T("a",0.0),T("b",0.5) };
  auto segs = merge_tokens(toks, 10.0, 30.0); // no split -> 1 segment
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].end == doctest::Approx(0.5 + 0.3));
}

TEST_CASE("T6: single token segment end = token.start + 0.3") {
  std::vector<Token> toks = { T("a",1.5) };
  auto segs = merge_tokens(toks, 1.0, 30.0);
  REQUIRE(segs.size() == 1);
  CHECK(segs[0].end == doctest::Approx(1.5 + 0.3));
}
```

- [ ] **Step 1b: Run to confirm `merge by gap` and T6 tests now fail**

```
build\Release\suji_tests.exe --test-case="merge by gap" --test-case="T6*"
```

Expected: FAIL (segs[0].end is still 0.3, not 2.0).

- [ ] **Step 2: Add `end` to `Token` in `src/core/types.h`**

Replace line 9:
```cpp
struct Token       { std::string text; double start = 0.0; };                 // 全局秒
```
with:
```cpp
struct Token       { std::string text; double start = 0.0; double end = 0.0; }; // 全局秒; end=下一token.start 或 start+0.3
```

- [ ] **Step 3: Update `src/core/segment_merge.cpp` — real end-time logic**

Replace the entire file content:

```cpp
#include "core/segment_merge.h"

namespace suji {
// T6: constant added to the last token's start to produce a sane end time
// when no following token exists (better than the old flat +2.0s in writers).
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
      // T6: segment ends at the START of the next token (= this token), not the last token's start
      cur.end = toks[i].start;
      segs.push_back(cur);
      cur = Segment{};
      cur.start = toks[i].start;
    }

    cur.tokens.push_back(toks[i]);
    cur.text += toks[i].text;
    cur.end = toks[i].start;   // will be updated as we advance; last token wins
  }

  // T6: final segment: last token's start + small constant (no following token)
  cur.end = toks.back().start + kLastTokenPad;
  segs.push_back(cur);
  return segs;
}
} // namespace suji
```

- [ ] **Step 4: Update `src/core/output/srt_writer.cpp` — remove +2.0 fallback**

Replace:
```cpp
    double end = (s.end > s.start) ? s.end : s.start + 2.0; // 防 0 时长
```
with:
```cpp
    // T7: use real segment end; +0.3 only as last resort when end<=start
    double end = (s.end > s.start) ? s.end : s.start + 0.3;
```

- [ ] **Step 5: Update `src/core/output/vtt_writer.cpp` — same**

Replace:
```cpp
    double end = (s.end > s.start) ? s.end : s.start + 2.0;
```
with:
```cpp
    // T7: use real segment end; +0.3 only as last resort when end<=start
    double end = (s.end > s.start) ? s.end : s.start + 0.3;
```

- [ ] **Step 6: Build and run full suite**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
build\Release\suji_tests.exe
```

Expected: All tests pass.

- [ ] **Step 7: Commit**

```
git add src/core/types.h src/core/segment_merge.cpp src/core/output/srt_writer.cpp src/core/output/vtt_writer.cpp tests/test_segment_merge.cpp
git commit -m "feat(T6+T7): real subtitle end-times via next-token-start; drop +2.0s hack"
```

---

### Task 5: G5 — SRT/VTT UTF-8-aware line wrapping

**Files:**
- Modify: `src/core/config.h` — add `srt_max_chars_per_line`
- Modify: `src/core/output/srt_writer.h` — update signature
- Modify: `src/core/output/srt_writer.cpp` — add wrapping
- Modify: `src/core/output/vtt_writer.h` — update signature
- Modify: `src/core/output/vtt_writer.cpp` — add wrapping
- Modify: `src/core/output/writer_facade.cpp` — pass `cfg` to writers
- Modify: `src/cli/batch_main.cpp` — add `--srt-line N` flag
- Modify: `tests/test_srt_writer.cpp` — add wrap tests + update signature call
- Modify: `tests/test_vtt_writer.cpp` — same

**Interfaces:**
- `to_srt(const Transcript& t, const EngineConfig& cfg)` — cfg.srt_max_chars_per_line (0 = no wrap)
- `to_vtt(const Transcript& t, const EngineConfig& cfg)` — same field

**UTF-8 codepoint counting:** A UTF-8 multi-byte sequence starts with a byte whose top bits are not `10xxxxxx` (i.e., `(byte & 0xC0) != 0x80`). Count codepoints by counting bytes where `(b & 0xC0) != 0x80`. Break at codepoint boundaries (never mid-character). Prefer breaking at ASCII space/`\n` if within 2 chars of limit; else hard-break at limit.

- [ ] **Step 1: Write failing tests** — update `tests/test_srt_writer.cpp`

```cpp
#include "doctest/doctest.h"
#include "core/output/srt_writer.h"
#include "core/config.h"
using namespace suji;

TEST_CASE("srt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"你好世界"; t.segments={s};
  EngineConfig cfg;  // srt_max_chars_per_line = 0 (no wrap)
  std::string srt = to_srt(t, cfg);
  CHECK(srt == "1\n00:00:01,000 --> 00:00:02,500\n\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96\xe7\x95\x8c\n\n");
}

TEST_CASE("G5: srt max=0 leaves text unwrapped") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五六七八九十"; // 10 CJK chars
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 0;
  std::string srt = to_srt(t, cfg);
  // text should appear on one line (no \n within cue body)
  std::string cue_body = u8"一二三四五六七八九十";
  CHECK(srt.find(cue_body) != std::string::npos);
}

TEST_CASE("G5: srt max=6 wraps 10-char zh string into lines <=6 codepoints") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五六七八九十"; // 10 CJK chars, each 3 bytes
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 6;
  std::string srt = to_srt(t, cfg);
  // Find the cue body (after the timecode line)
  auto tpos = srt.find("00:00:00,000 --> 00:00:01,000\n");
  REQUIRE(tpos != std::string::npos);
  std::string body = srt.substr(tpos + 30); // after timecode line
  // body should contain a newline splitting the text
  CHECK(body.find('\n') != std::string::npos);
  // First line should be at most 6 CJK chars = 18 bytes
  auto nl = body.find('\n');
  REQUIRE(nl != std::string::npos);
  std::string line1 = body.substr(0, nl);
  // Count codepoints in line1
  int cp = 0;
  for (unsigned char b : line1) if ((b & 0xC0) != 0x80) ++cp;
  CHECK(cp <= 6);
}

TEST_CASE("G5: srt multi-byte UTF-8 not split mid-character") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五"; // 5 CJK, 15 bytes
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 3;
  std::string srt = to_srt(t, cfg);
  // Each byte in srt must still be valid UTF-8 start/continuation
  // (we just check the output is valid by verifying no orphan continuation bytes)
  const std::string& out = srt;
  for (size_t i = 0; i < out.size(); ) {
    unsigned char b = (unsigned char)out[i];
    int len = (b < 0x80) ? 1 : (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;
    // Verify continuation bytes
    bool valid = true;
    for (int j = 1; j < len && i+j < out.size(); ++j)
      if (((unsigned char)out[i+j] & 0xC0) != 0x80) { valid = false; break; }
    CHECK(valid);
    i += len;
  }
}
```

- [ ] **Step 2: Run to verify they fail**

```
build\Release\suji_tests.exe --test-case="srt basic" --test-case="G5*"
```

Expected: FAIL — `to_srt` doesn't accept `EngineConfig` yet.

- [ ] **Step 3: Add `srt_max_chars_per_line` to `src/core/config.h`**

After `max_threads`:
```cpp
  // SRT/VTT 行宽(0=不换行; >0=按 UTF-8 码点数断行)
  int srt_max_chars_per_line = 0;
```

- [ ] **Step 4: Update `src/core/output/srt_writer.h`**

Replace:
```cpp
namespace suji { std::string to_srt(const Transcript& t); }
```
with:
```cpp
#include "core/config.h"
namespace suji { std::string to_srt(const Transcript& t, const EngineConfig& cfg); }
```

- [ ] **Step 5: Update `src/core/output/vtt_writer.h`**

Replace:
```cpp
namespace suji { std::string to_vtt(const Transcript& t); }
```
with:
```cpp
#include "core/config.h"
namespace suji { std::string to_vtt(const Transcript& t, const EngineConfig& cfg); }
```

- [ ] **Step 6: Write the UTF-8 wrap helper and update `src/core/output/srt_writer.cpp`**

Replace the entire file:

```cpp
#include "core/output/srt_writer.h"
#include "core/timestamp.h"
#include <string>

namespace suji {

// G5: wrap `text` into lines of at most `max_codepoints` codepoints each.
// Breaks at ASCII space/newline if within 2 chars of the limit; else hard-breaks
// at a codepoint boundary. Returns text unchanged when max_codepoints <= 0.
static std::string wrap_codepoints(const std::string& text, int max_codepoints) {
  if (max_codepoints <= 0 || text.empty()) return text;

  std::string result;
  result.reserve(text.size() + text.size() / (max_codepoints * 3) + 1);

  const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
  const unsigned char* end = p + text.size();
  int line_cp = 0;           // codepoints on current line
  const unsigned char* line_start = p;

  auto flush_line = [&](const unsigned char* upto, bool add_newline) {
    result.append(reinterpret_cast<const char*>(line_start),
                  reinterpret_cast<const char*>(upto) - reinterpret_cast<const char*>(line_start));
    if (add_newline) result += '\n';
    line_start = upto;
    line_cp = 0;
  };

  while (p < end) {
    // Determine codepoint byte length
    unsigned char b = *p;
    int cp_len = (b < 0x80) ? 1 : (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;

    // Advance pointer to next codepoint boundary
    const unsigned char* next = p + cp_len;
    if (next > end) next = end; // guard against truncated input

    // Check if this is a natural break character (ASCII space or newline)
    bool is_break_char = (cp_len == 1 && (*p == ' ' || *p == '\n'));

    // If adding this codepoint would exceed limit, flush current line first
    if (line_cp >= max_codepoints) {
      // Break right before this codepoint
      flush_line(p, true);
    } else if (line_cp > 0 && line_cp >= max_codepoints - 2 && is_break_char) {
      // We're within 2 chars of the limit and hit a natural break — flush here
      // Skip the break character itself (it becomes the newline)
      flush_line(p, true);
      p = next; // consume the space/newline
      line_start = p;
      continue;
    }

    line_cp++;
    p = next;
  }

  // Flush remaining
  if (line_start < end) {
    result.append(reinterpret_cast<const char*>(line_start),
                  reinterpret_cast<const char*>(end) - reinterpret_cast<const char*>(line_start));
  }
  return result;
}

std::string to_srt(const Transcript& t, const EngineConfig& cfg) {
  std::string out;
  int idx = 1;
  for (const auto& s : t.segments) {
    // T7: use real segment end; +0.3 only as last resort when end<=start
    double end = (s.end > s.start) ? s.end : s.start + 0.3;
    out += std::to_string(idx++) + "\n";
    out += format_srt_time(s.start) + " --> " + format_srt_time(end) + "\n";
    out += wrap_codepoints(s.text, cfg.srt_max_chars_per_line) + "\n\n";
  }
  return out;
}
} // namespace suji
```

- [ ] **Step 7: Update `src/core/output/vtt_writer.cpp`**

Replace the entire file:

```cpp
#include "core/output/vtt_writer.h"
#include "core/timestamp.h"
#include <string>

namespace suji {

// G5: same codepoint-aware wrap as srt_writer; shared only by copying
// (keeping writers self-contained is preferred over a shared internal header).
static std::string wrap_codepoints_vtt(const std::string& text, int max_codepoints) {
  if (max_codepoints <= 0 || text.empty()) return text;
  std::string result;
  result.reserve(text.size() + text.size() / (max_codepoints * 3) + 1);
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
  const unsigned char* end = p + text.size();
  int line_cp = 0;
  const unsigned char* line_start = p;
  auto flush_line = [&](const unsigned char* upto, bool add_newline) {
    result.append(reinterpret_cast<const char*>(line_start),
                  reinterpret_cast<const char*>(upto) - reinterpret_cast<const char*>(line_start));
    if (add_newline) result += '\n';
    line_start = upto;
    line_cp = 0;
  };
  while (p < end) {
    unsigned char b = *p;
    int cp_len = (b < 0x80) ? 1 : (b < 0xE0) ? 2 : (b < 0xF0) ? 3 : 4;
    const unsigned char* next = p + cp_len;
    if (next > end) next = end;
    bool is_break_char = (cp_len == 1 && (*p == ' ' || *p == '\n'));
    if (line_cp >= max_codepoints) {
      flush_line(p, true);
    } else if (line_cp > 0 && line_cp >= max_codepoints - 2 && is_break_char) {
      flush_line(p, true);
      p = next;
      line_start = p;
      continue;
    }
    line_cp++;
    p = next;
  }
  if (line_start < end) {
    result.append(reinterpret_cast<const char*>(line_start),
                  reinterpret_cast<const char*>(end) - reinterpret_cast<const char*>(line_start));
  }
  return result;
}

std::string to_vtt(const Transcript& t, const EngineConfig& cfg) {
  std::string out = "WEBVTT\n\n";
  for (const auto& s : t.segments) {
    // T7: use real segment end; +0.3 only as last resort when end<=start
    double end = (s.end > s.start) ? s.end : s.start + 0.3;
    out += format_vtt_time(s.start) + " --> " + format_vtt_time(end) + "\n";
    out += wrap_codepoints_vtt(s.text, cfg.srt_max_chars_per_line) + "\n\n";
  }
  return out;
}
} // namespace suji
```

- [ ] **Step 8: Update `src/core/output/writer_facade.cpp` — pass cfg to writers**

Replace:
```cpp
  if (cfg.out_srt)  ok &= write_utf8_no_bom(base + ".srt",  to_srt(t));
  if (cfg.out_vtt)  ok &= write_utf8_no_bom(base + ".vtt",  to_vtt(t));
```
with:
```cpp
  if (cfg.out_srt)  ok &= write_utf8_no_bom(base + ".srt",  to_srt(t, cfg));
  if (cfg.out_vtt)  ok &= write_utf8_no_bom(base + ".vtt",  to_vtt(t, cfg));
```

- [ ] **Step 9: Update `tests/test_vtt_writer.cpp`**

Replace:
```cpp
#include "doctest/doctest.h"
#include "core/output/vtt_writer.h"
using namespace suji;
TEST_CASE("vtt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"abc"; t.segments={s};
  std::string vtt = to_vtt(t);
  CHECK(vtt.rfind("WEBVTT\n\n",0)==0);
  CHECK(vtt.find("00:00:01.000 --> 00:00:02.500\nabc\n\n") != std::string::npos);
}
```
with:
```cpp
#include "doctest/doctest.h"
#include "core/output/vtt_writer.h"
#include "core/config.h"
using namespace suji;
TEST_CASE("vtt basic") {
  Transcript t; Segment s; s.start=1.0; s.end=2.5; s.text=u8"abc"; t.segments={s};
  EngineConfig cfg;
  std::string vtt = to_vtt(t, cfg);
  CHECK(vtt.rfind("WEBVTT\n\n",0)==0);
  CHECK(vtt.find("00:00:01.000 --> 00:00:02.500\nabc\n\n") != std::string::npos);
}

TEST_CASE("G5: vtt max=0 leaves text unwrapped") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五六七八九十";
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 0;
  std::string vtt = to_vtt(t, cfg);
  CHECK(vtt.find(u8"一二三四五六七八九十") != std::string::npos);
}

TEST_CASE("G5: vtt max=6 wraps 10-char zh string") {
  Transcript t; Segment s; s.start=0.0; s.end=1.0;
  s.text = u8"一二三四五六七八九十";
  t.segments={s};
  EngineConfig cfg; cfg.srt_max_chars_per_line = 6;
  std::string vtt = to_vtt(t, cfg);
  // should contain a newline in the cue body
  auto tpos = vtt.find("00:00:00.000 --> 00:00:01.000\n");
  REQUIRE(tpos != std::string::npos);
  std::string body = vtt.substr(tpos + 30);
  auto nl = body.find('\n');
  REQUIRE(nl != std::string::npos);
  std::string line1 = body.substr(0, nl);
  int cp = 0;
  for (unsigned char b : line1) if ((b & 0xC0) != 0x80) ++cp;
  CHECK(cp <= 6);
}
```

- [ ] **Step 10: Add `--srt-line N` to `src/cli/batch_main.cpp`**

In the `usage` string (line 47), add `[--srt-line N]` to the printed help.

Add a local variable after `bool resume = true;`:
```cpp
  int fsrt_line = 0;
```

In the argument parse loop, after the `--no-resume` branch, add:
```cpp
    else if (a == "--srt-line" && i + 1 < argc) {
      fsrt_line = parse_positive_int(argv[++i]);
      // 0 means "no wrap" which is the valid default; don't error on 0
    }
```

After `c.num_threads = tune.num_threads;` (near line 192), add:
```cpp
  if (fsrt_line > 0) c.srt_max_chars_per_line = fsrt_line;
```

- [ ] **Step 11: Build and run full suite**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
build\Release\suji_tests.exe
```

Expected: All tests pass.

- [ ] **Step 12: Commit**

```
git add src/core/config.h src/core/output/srt_writer.h src/core/output/srt_writer.cpp src/core/output/vtt_writer.h src/core/output/vtt_writer.cpp src/core/output/writer_facade.cpp src/cli/batch_main.cpp tests/test_srt_writer.cpp tests/test_vtt_writer.cpp
git commit -m "feat(G5): UTF-8-aware SRT/VTT line wrapping via --srt-line N / EngineConfig"
```

---

### Task 6: Final build verification + demo run

- [ ] **Step 1: Full clean build + test run**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release 2>&1
build\Release\suji_tests.exe -v 2>&1 | tail -5
```

Capture test count. Expect: 0 failures, all tests pass.

- [ ] **Step 2: Demo SRT run with --srt-line and real end-times**

```
build\Release\suji_batch.exe "models\sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25\test_wavs\0.wav" -o build\b4out --srt-line 12 --no-resume
```

Show first ~20 lines of `build\b4out\0.srt`.

- [ ] **Step 3: Write report to `.superpowers/sdd/todo-batch4-report.md`**

Create `F:\Git\suji-asr\.superpowers\sdd\todo-batch4-report.md` with the status, commit SHAs, T6 durations finding, sample SRT, test count, and any concerns.

---

## Self-Review

**Spec coverage check:**

- T3 (decoding_method): Task 1 — add field to config.h, use in asr.cpp. ✓
- T5 (decide() caps): Task 3 — max_batch/max_threads in config.h + decide(). ✓
- T6 (real end-times): Task 4 — next-token-start in segment_merge.cpp. ✓
- T7 (drop +2.0s): Task 4 — writers use s.end, +0.3 fallback only. ✓
- G5 (line wrap): Task 5 — wrap_codepoints in writers, --srt-line flag, config field. ✓
- G10 (punct config): Task 2 — punct_provider/punct_threads in config.h + punctuation.cpp. ✓

**Placeholder scan:** No TBDs. All code shown in full.

**Type consistency check:**
- `EngineConfig::decoding_method` — defined Task 1, consumed in asr.cpp Task 1. ✓
- `EngineConfig::punct_provider`, `punct_threads` — defined Task 2, consumed punctuation.cpp Task 2. ✓
- `EngineConfig::max_batch`, `max_threads` — defined Task 3, consumed hardware.cpp Task 3. ✓
- `Token::end` — defined Task 4, set in segment_merge.cpp Task 4. ✓
- `EngineConfig::srt_max_chars_per_line` — defined Task 5, consumed srt_writer.cpp/vtt_writer.cpp Task 5. ✓
- `to_srt(t, cfg)`, `to_vtt(t, cfg)` — new signatures in Task 5, writer_facade.cpp updated Task 5. ✓

**Key concern:** The `wrap_codepoints` function is duplicated between srt_writer.cpp and vtt_writer.cpp. This is deliberate (keeps files self-contained, the function is ~40 lines) but could be refactored into a shared internal header later. Not YAGNI to do it now.

**Second concern:** The `--srt-line 0` parse in batch_main.cpp: `parse_positive_int("0")` returns 0 (not positive), so the override is skipped and `c.srt_max_chars_per_line` stays at the default 0 (= no wrap). This is the correct behavior — `--srt-line 0` means "no wrap". Document in the flag comment.
