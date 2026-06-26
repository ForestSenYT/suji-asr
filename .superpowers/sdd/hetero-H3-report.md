# H3 — Heterogeneous CPU+GPU dual-consumer engine — implementation report

Branch: `feat/hetero-engine`  •  Commits: `d4fc249`, `5b2a872`  •  Date: 2026-06-26

## Status: DONE — full suite green (63/63), end-to-end hetero verified on 3 real clips.

---

## 1. Design as built

### Public dispatch (signature UNCHANGED)
`src/core/batch_engine.cpp` — the old `transcribe_batch_files` body is now the
file-static `transcribe_batch_files_single(...)`. The public entry is a thin
dispatcher:

```cpp
std::vector<FileResult> transcribe_batch_files(...){
  if(tune.provider == Provider::Hetero)
    return transcribe_batch_files_hetero(inputs, cfg, tune, cb, cancel);
  return transcribe_batch_files_single(inputs, cfg, tune, cb, cancel);
}
```

CLI (`suji_batch`/`suji_cli`) and GUI call sites are untouched — they still call
`transcribe_batch_files(...)`. H4/H5 will expose `hetero` at the CLI/GUI.

### `transcribe_batch_files_hetero(...)`
- **Two recognizers, CPU first then CUDA** (deterministic, per H0's gating finding):
  `cpu_cfg.provider=Cpu; num_threads=tune.cpu_asr_threads`;
  `gpu_cfg.provider=Cuda; num_threads=1; cuda_dll_dir=cfg.cuda_dll_dir`.
- **Graceful degradation**: `cok=cpu_asr.ok(), gok=gpu_asr.ok()`.
  - both false → all files `ok=false err="ASR init failed"`, return.
  - only one ok → only that consumer thread is spawned (the dead engine is never
    touched). Logged via `log_err`.
- **Shared `BoundedQueue<SegTask>`** with R4 cap:
  `cap = max(4, (cpu_batch+gpu_batch)*4, in_flight_files*8)`. On the target
  machine (cpu_batch=4, gpu_batch=30, in_flight=4) → `cap = max(4,136,32) = 136`,
  keeping the queue saturated so the GPU's opportunistic `try_pop` can't starve CPU.
- **Two lock-free token sinks**: `tok_cpu(N)`, `tok_gpu(N)`. Each consumer writes
  ONLY its own sink on the hot path — no shared-state contention while transcribing.
- **Producers**: identical to the single path — `next_file.fetch_add` → cancel
  check → `decode_to_pcm(...,cancel)` → cancel check → `Vad vad(cfg); vad.segment(ab,cancel)`
  → push `SegTask`. `tune.in_flight_files` producer threads.
- **One reusable consumer lambda** `consume(Asr& asr, int batch_max, sink&)`:
  `pop` one + `try_pop` up to `batch_max` → build `Asr::SegView` → `transcribe_batch`
  → on size match route tokens by `file_id` into its sink, `samples_done += ...`,
  throttled live cb. CPU consumer uses `(cpu_asr, cpu_batch, tok_cpu)`; GPU uses
  `(gpu_asr, gpu_batch, tok_gpu)`. **Each recognizer handle is touched by exactly
  one thread.**
- **Join order**: join all producers → `queue.close()` → join both consumers.
- **Finalize per file**: `toks = tok_cpu[i] + tok_gpu[i]` → `std::sort` by `.start`
  → `merge_tokens` → punctuate → progress cb.

Shared helpers `route_batch_tokens(...)` and `mark_batch_failed(...)` are used by
both the single and hetero paths so the R3 handling and token routing are identical.

---

## 2. Concurrency-safety argument

1. **No double-processing.** A `SegTask` lives in the queue and is removed by
   exactly one successful `pop`/`try_pop` (the `BoundedQueue` mutex makes the
   front-pop atomic). Both consumers only ever *remove* tasks; neither requeues.
   Therefore the set of tasks each consumer sees is disjoint and their union is
   the full set. Proven empirically by the pure test (K=5000, two consumers,
   `intersection==∅`, `union==K`).
2. **No handle sharing.** Each `Asr` handle is captured by a single consumer
   lambda and that lambda runs on one thread. This is exactly the H0 gating
   invariant ("each recognizer handle touched by exactly ONE thread"), which
   passed at 50 rounds. The smoke test still runs every build as a regression
   guard (12 rounds, both engines concurrent).
3. **Lock-free hot path.** Token writes go to per-engine `sink[file_id]`; the two
   consumers never write the same `std::vector`, so no lock is needed while
   transcribing. `samples_done` / `files_done` / `next_file` are `std::atomic`.
4. **Shared `results[]` writes are guarded.** Producers (decode/VAD errors,
   `produced_complete`) and consumers (`mark_batch_failed`) write `results[]`
   only under `err_mu`.
5. **The throttled live cb** reads `consumer_last_cb` from two threads; it is
   serialized by a tiny `cb_lock` CAS so only the lock-holder touches the
   timestamp and invokes `cb`. (Cb is best-effort progress; a missed tick is fine.)
6. **Cancel teardown can't hang.** On cancel a consumer calls `queue.close()`
   and breaks; `close()` wakes all blocked producers (`notfull_`) and the other
   blocked consumer (`notempty_`), so every thread unblocks and joins. Proven by
   the pure cancel test (`doctest::timeout(10)`) and the model-based hetero cancel
   test (`timeout(120)`), both of which join without hanging.

No real concurrency hazard was found that would make any requirement unsafe, so
no STOP was needed.

---

## 3. R3 (+T12) — no silent data loss

- **Root cause** (`asr.cpp`): on `SherpaOnnxCreateOfflineStream`==NULL (e.g. VRAM
  pressure) `transcribe_batch` destroyed the partial streams and returned the
  pre-sized **`segs.size()`-length all-empty** `out`. So `res.size()==batch.size()`,
  the caller's size-mismatch guard never fired, and the whole batch was recorded
  as a *successful empty result* while the file stayed `ok==true` → silent dropped
  text. The hetero CUDA consumer near the VRAM ceiling would double-expose this.
- **Fix (`asr.cpp`, commit d4fc249)**: return an **empty vector** (`return {};`) on
  stream-creation failure, making "stream-creation failure" distinguishable from
  "legitimately empty results" (which is still `segs.size()` entries with empty text).
- **Consumer fix (both paths)**: the `res.size()!=batch.size()` branch no longer
  silently `continue`s. It calls `mark_batch_failed(...)`, which (a) `log_err`s the
  wrong-size event and (b) marks **every distinct `file_id`** in that batch
  `ok=false err="transcribe failed"`, guarded by `err_mu`. A failed batch is never
  recorded as a successful empty result.

---

## 4. R6 — cancel correctness

- Added `std::vector<char> produced_complete(N,0)`. A producer sets
  `produced_complete[fi]=1` (under `err_mu`) **only after pushing ALL of that
  file's VAD segments**.
- Finalize (both single and hetero) replaces the old heuristic
  `results[i].ok && file_tokens[i].empty()` with
  `results[i].ok && !produced_complete[i]`: a cancelled file that received some
  segments but whose production never completed is now `ok=false err="cancelled"`,
  so a **truncated transcript is never reported ok**. Files that completed
  production (or were never cancelled) keep their real transcript.

---

## 5. Test results

Full suite (`build\Release\suji_tests.exe`): **63 test cases, 252 assertions, 0
failed** (baseline 57 + 6 new). `/W4` clean on all changed files; build green.

New tests:
- **No double-processing** (pure queue, no models): K=5000, two consumers via
  pop/try_pop → `intersection==∅`, `union==K`. PASS.
- **Token merge/sort**: interleaved `tok_cpu`+`tok_gpu`, sorted by `.start`, equals
  the single-array baseline through `merge_tokens` (same seg count/text/start/end).
  PASS.
- **Cancel releases both consumers** (pure, timeout 10s): blocked producer + cancel
  + close → both consumers join, no hang. PASS.
- **Graceful degradation** (real models): `cuda_dll_dir="Z:/...bad"` → `gpu_asr.ok()`
  false → CPU-only consumer completes all 3 files `ok` with non-empty transcripts.
  PASS.
- **Hetero cancel safety** (real models, timeout 120s): 6 clips, cancel after 1.2s →
  worker joins promptly, ≥1 file `ok=false err="cancelled"`, and no `ok` file has an
  empty transcript (no truncated-but-ok). PASS.
- **End-to-end hetero** (real models): see below.

### End-to-end hetero output (3 real zh/en clips)
```
file[0] ok=true segs=3 text='昨天是MAY。TODAY IS礼拜二。THEDAY AFTER TOMORROW是星期三。'
file[1] ok=true segs=1 text='这是第一种。第二种叫呃与ALWAYSISE ALWAYS什么意思啊？'
file[2] ok=true segs=1 text='这个是平繁的啊，不认识记下来FREQUENTLY平繁的。'
HETERO E2E: 3 files, total_segs=5, provider=hetero, no crash — dual-consumer (CPU+CUDA)
```
All 3 files `ok`, segs>0 each, total_segs=5, no crash. CUDA runtime present on this
box (`cuda_dll_dir()` non-empty) so both consumers ran.

---

## 6. Concerns / notes

- **Throughput (H8) not measured here** — H3 is correctness only; the brief defers
  benchmarking to H8. On this dev box (2080 + int8) CPU is the faster engine, so
  hetero throughput vs CPU-only is an H8 question.
- **Run-to-run non-determinism** (H7): hetero token output is non-deterministic by
  design (segment→engine assignment varies with timing; CPU vs CUDA EP differ
  numerically on the int8 model). Tests assert structure (ok/segs/non-empty), not
  exact text.
- **CLI/GUI** still cannot select `hetero` (H4/H5). The dispatcher only activates
  hetero when `tune.provider==Provider::Hetero`, which today only `decide()` sets.
- Cosmetic: line-ending (LF→CRLF) git warnings on commit; harmless.

Report path: `F:\Git\suji-asr\.superpowers\sdd\hetero-H3-report.md`

---

## Fix: R6 consumption tracking

**Bug (R6 IMPORTANT-1):** `produced_complete[fi]` only tracks push-completion. If cancel fires after a file is fully pushed but while tail segments remain queued or are the popped-but-discarded `first` on the cancel break, those segments are silently dropped yet `finalize` kept the file `ok=true` — truncated transcript reported as ok, directly violating R6.

**Fix (`src/core/batch_engine.cpp`):**
- Added `std::unique_ptr<std::atomic<int>[]> seg_pending(new std::atomic<int>[N])` in both `transcribe_batch_files_single` and `transcribe_batch_files_hetero` (avoids `vector<atomic>` copy-constructor issue under /W4).
- **Producer**: `seg_pending[fi].fetch_add(1)` before each `queue.push()`.
- **Consumer(s)**: `seg_pending[b.file_id].fetch_sub(1)` after `route_batch_tokens` for each successfully processed batch element. The discarded `first` on the cancel path is intentionally NOT decremented — its `seg_pending` count stays elevated.
- **Finalize** (both paths): guard changed from `!produced_complete[i]` to `(seg_pending[i].load() > 0 || !produced_complete[i])`. The OR covers: (a) tail segments still in queue or popped-but-discarded, (b) push was itself interrupted. Only overrides files that are currently `ok` (real decode/VAD/transcribe failures are not clobbered). A clean (non-cancelled) run leaves `seg_pending[i]==0` for every file — verified by test.

**Tests added (`tests/test_hetero_engine.cpp`):**
- `"R6: partial consumption on cancel is classified cancelled, not ok"` — simulates K=10 segments pushed (produced_complete=true), 3 consumed, cancel fires. Proves the OLD logic (`!produced_complete` only) returns `false` (wrong — bug), and the NEW logic (`seg_pending > 0 || !produced_complete`) returns `true` (correct — R6 upheld).
- `"R6: clean run leaves seg_pending zero and files stay ok"` — 3 files with varying segment counts all fully consumed, no cancel. Asserts `seg_pending[i]==0` for all i and no file is reclassified.

**Full suite: 65/65 passed, 261 assertions, 0 failed** (was 63/63 before this fix; +2 new R6 tests). Build is /W4 pristine. Hetero e2e and smoke tests pass unchanged.
