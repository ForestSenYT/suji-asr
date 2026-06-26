# Live Progress Fix — batch_engine + GUI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the GUI freeze where files stay at "待处理" with no progress for the entire transcription then jump to "完成" in one frame.

**Architecture:** (1) Emit the `cb` callback from the consumer thread after each processed batch (throttled to ~150 ms), so the GUI sees live updates. (2) In `onWorkerStarted`, flip every "待处理" row to "处理中" and switch the progress bar to indeterminate mode. (3) In `onWorkerProgress`, stop calling `setValue` (bar is indeterminate). (4) In `onWorkerFinished`, restore the bar to determinate + 100 before the final status text. (5) Add an integration test asserting the callback fires more times than there are files.

**Tech Stack:** C++17, MSVC `/utf-8` `/W4`, Qt6, doctest, sherpa-onnx, CMake 4.3.3

## Global Constraints

- C++17, MSVC, `/utf-8`, `/W4` pristine — no new warnings
- Chinese string literals OK (source files already use them under `/utf-8`)
- Do NOT change `BatchProgress` struct fields or `transcribe_batch_files` public signature
- Keep finalize-loop `cb` call (line 97 of `batch_engine.cpp`) unchanged
- All existing 40 test cases must stay green
- Build command: `F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release`
- Test runner: `build\Release\suji_tests.exe`

---

### Task 1: Emit live `cb` from consumer thread in `batch_engine.cpp`

**Files:**
- Modify: `F:\Git\suji-asr\src\core\batch_engine.cpp:57-77`

**Interfaces:**
- Produces: `cb` now fires mid-transcription (not only in finalize loop on line 97)

- [ ] **Step 1: Add throttled live callback after batch processing**

In `batch_engine.cpp`, the consumer lambda (lines 57–77) ends just before the closing `}`. Insert this code after line 75 (`samples_done += batch[i].samples.size();`) and before line 76 (closing `}` of for-loop) — but OUTSIDE the per-element for loop, i.e., at the END of the consumer's while-body:

The consumer lambda currently looks like:
```cpp
  std::thread consumer([&]{
    SegTask first;
    while(queue.pop(first)){
      if(cancel && cancel->is_cancelled()){ queue.close(); break; }
      std::vector<SegTask> batch; batch.push_back(std::move(first));
      SegTask more;
      while((int)batch.size() < tune.batch && queue.try_pop(more)) batch.push_back(std::move(more));
      std::vector<Asr::SegView> views; views.reserve(batch.size());
      for(auto& b : batch) views.push_back({b.samples.data(),(int)b.samples.size()});
      auto res = asr.transcribe_batch(views);
      if (res.size() != batch.size()) continue;
      for(size_t i=0;i<batch.size();++i){
        double base = (double)batch[i].start_sample/16000.0;
        for(size_t k=0;k<res[i].tokens.size() && k<res[i].timestamps.size();++k){
          Token tk; tk.text=res[i].tokens[k]; tk.start=base+res[i].timestamps[k];
          file_tokens[batch[i].file_id].push_back(tk);
        }
        samples_done += batch[i].samples.size();
      }
    }
  });
```

Replace the consumer lambda with this version that adds throttled live cb:
```cpp
  auto consumer_last_cb = std::chrono::steady_clock::time_point{};  // zero = never fired
  std::thread consumer([&]{
    SegTask first;
    while(queue.pop(first)){
      if(cancel && cancel->is_cancelled()){ queue.close(); break; }
      std::vector<SegTask> batch; batch.push_back(std::move(first));
      SegTask more;
      while((int)batch.size() < tune.batch && queue.try_pop(more)) batch.push_back(std::move(more));
      std::vector<Asr::SegView> views; views.reserve(batch.size());
      for(auto& b : batch) views.push_back({b.samples.data(),(int)b.samples.size()});
      auto res = asr.transcribe_batch(views);
      if (res.size() != batch.size()) continue;
      for(size_t i=0;i<batch.size();++i){
        double base = (double)batch[i].start_sample/16000.0;
        for(size_t k=0;k<res[i].tokens.size() && k<res[i].timestamps.size();++k){
          Token tk; tk.text=res[i].tokens[k]; tk.start=base+res[i].timestamps[k];
          file_tokens[batch[i].file_id].push_back(tk);
        }
        samples_done += batch[i].samples.size();
      }
      // live progress: throttle to ~150 ms so we don't flood the GUI
      if(cb){
        auto now = std::chrono::steady_clock::now();
        if(consumer_last_cb == std::chrono::steady_clock::time_point{} ||
           std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_cb).count() >= 150){
          consumer_last_cb = now;
          BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load();
          bp.audio_seconds_done=(double)samples_done.load()/16000.0;
          cb(bp);
        }
      }
    }
  });
```

Note: `consumer_last_cb` is declared OUTSIDE the thread lambda (before `std::thread consumer`) so it can be captured by reference — but it is only ever touched by the consumer thread (no race). The `chrono` header is already included via `<thread>` transitively in MSVC but to be safe, `<chrono>` should already be present — check the includes; if not, add `#include <chrono>`.

- [ ] **Step 2: Verify `<chrono>` is included**

Check top of `batch_engine.cpp`. Currently includes: `<thread>`, `<atomic>`, `<mutex>`, `<algorithm>`. MSVC's `<thread>` does NOT guarantee `<chrono>`. Add `#include <chrono>` after `#include <algorithm>` if not already there.

- [ ] **Step 3: Build to verify no compile errors**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
```
Expected: zero errors, zero new warnings about `consumer_last_cb` or chrono.

- [ ] **Step 4: Run existing tests to confirm no regressions**

```
build\Release\suji_tests.exe
```
Expected: all existing tests pass (baseline: 40 cases). Look especially at `batch transcribe multiple files (CPU)` and `batch isolates a bad file`.

- [ ] **Step 5: Commit**

```
git add src/core/batch_engine.cpp
git commit -m "fix: emit live progress cb from consumer thread (throttled 150ms)"
```

---

### Task 2: Add integration test proving live mid-run callback

**Files:**
- Modify: `F:\Git\suji-asr\tests\integration\test_batch_engine.cpp`

**Interfaces:**
- Consumes: `transcribe_batch_files`, `BatchProgress`, `EngineConfig`, `AutoTune`, `Provider` (all defined in `core/batch_engine.h` and `core/config.h`)
- Produces: new TEST_CASE `"live progress fires during transcription (not only at finalize)"` that asserts `cb_count > (int)inputs.size()`

- [ ] **Step 1: Write the failing test**

Append to `tests/integration/test_batch_engine.cpp` after line 28:
```cpp
TEST_CASE("live progress fires during transcription (not only at finalize)" * doctest::timeout(300)){
  // 0.wav has multiple VAD segments → multiple consumer batches → consumer cb fires > 1 time
  // finalize loop fires exactly 1 time (one file). So cb_count > 1 proves live emission.
  std::string w=md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  std::vector<std::string> inputs={ w+"0.wav" };
  AutoTune tune; tune.provider=Provider::Cpu; tune.batch=1; tune.in_flight_files=1; tune.num_threads=4;
  // batch=1 forces one consumer cb call per VAD segment batch, maximizing cb count
  int cb_count = 0;
  auto res = transcribe_batch_files(inputs, cfg(), tune, [&](const BatchProgress& b){
    (void)b;
    ++cb_count;
  });
  REQUIRE(res.size()==1);
  CHECK(res[0].ok);
  // cb_count must exceed inputs.size() (1), proving callback fired during transcription
  // (finalize loop fires exactly 1 time; consumer fires once per VAD batch)
  CHECK(cb_count > (int)inputs.size());
}
```

- [ ] **Step 2: Build**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
```
Expected: compiles clean.

- [ ] **Step 3: Run tests — expect the new test to PASS**

```
build\Release\suji_tests.exe -tc="live progress fires during transcription*"
```
Expected: PASS. If it fails with `cb_count == 1`, the consumer cb from Task 1 was not implemented correctly.

- [ ] **Step 4: Run full suite**

```
build\Release\suji_tests.exe
```
Expected: all tests pass (now 41 cases).

- [ ] **Step 5: Commit**

```
git add tests/integration/test_batch_engine.cpp
git commit -m "test: assert live cb fires during transcription (not only finalize)"
```

---

### Task 3: GUI — show "处理中" row state + indeterminate progress bar

**Files:**
- Modify: `F:\Git\suji-asr\src\gui\main_window.cpp:316-372`

**Interfaces:**
- Consumes: `m_model`, `m_progress`, `m_btnStart`, `m_btnCancel`, `setStatusText`, `setRowStatus` — all defined in `main_window.h` and used elsewhere in the file
- Produces: `onWorkerStarted` flips rows + sets indeterminate bar; `onWorkerProgress` skips `setValue`; `onWorkerFinished` restores determinate bar

**Changes needed:**

**`onWorkerStarted` (lines 316–321):** After the existing `setStatusText(...)` call, add a loop that flips "待处理" rows to "处理中" and puts the bar into indeterminate (busy) mode. Replace the function body:

```cpp
void MainWindow::onWorkerStarted(QString provider, int filesTotal)
{
    setStatusText(tr("正在用 %1 转写 %2 个文件…")
        .arg(provider.toUpper())
        .arg(filesTotal));

    // Flip every pending row to "in-progress"
    const int n = m_model->rowCount();
    for (int r = 0; r < n; ++r) {
        if (m_model->item(r, ColStatus)->text() == tr("待处理"))
            setRowStatus(r, tr("处理中"));
    }
    // Indeterminate mode: min==max==0 makes Qt show a busy animation
    m_progress->setRange(0, 0);
}
```

**`onWorkerProgress` (lines 323–336):** Remove the `m_progress->setValue(...)` call (bar is indeterminate while running). Keep the status text update. Replace the function body:

```cpp
void MainWindow::onWorkerProgress(int filesDone, int filesTotal, double audioSec)
{
    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_startTime).count();
    double throughput = (elapsed > 0.0) ? (audioSec / elapsed) : 0.0;

    setStatusText(tr("处理中 %1/%2  %3 倍速")
        .arg(filesDone)
        .arg(filesTotal)
        .arg(throughput, 0, 'f', 1));
}
```

**`onWorkerFinished` (lines 358–372):** Restore the bar to determinate FIRST, then existing logic. Replace the function body:

```cpp
void MainWindow::onWorkerFinished(int ok, int failed, int cancelled, double wallSec)
{
    // Restore determinate mode before setting value
    m_progress->setRange(0, 100);
    m_progress->setValue(100);
    m_btnStart->setEnabled(true);
    m_btnCancel->setEnabled(false);

    double elapsed = (wallSec > 0.0) ? wallSec : 1.0;
    setStatusText(
        tr("完成: %1 成功  %2 失败  %3 取消  %4 秒")
            .arg(ok)
            .arg(failed)
            .arg(cancelled)
            .arg(elapsed, 0, 'f', 1)
    );
}
```

- [ ] **Step 1: Apply the three function changes to `main_window.cpp`**

Edit `F:\Git\suji-asr\src\gui\main_window.cpp` replacing each function body as shown above.

- [ ] **Step 2: Build**

```
F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
```
Expected: zero errors. The `ColStatus` enum member must be in scope — verify it exists in `main_window.cpp` (search for `ColStatus` — it's used in `onWorkerFileResult`).

- [ ] **Step 3: Run full test suite**

```
build\Release\suji_tests.exe
```
Expected: all tests pass (41 cases). GUI changes don't affect unit/integration tests.

- [ ] **Step 4: Commit**

```
git add src/gui/main_window.cpp
git commit -m "fix: indeterminate progress bar + 处理中 row status during transcription"
```

---

### Task 4: End-to-end GUI proof via `--selftest-gui`

**Files:**
- No code changes — this is a verification step

**Goal:** Confirm row0 shows "处理中" while the run is in progress (not "待处理"), then "完成" when done.

- [ ] **Step 1: Ensure test video exists**

Check if `F:\Git\suji-asr\build\test_video.mp4` exists. If not, create it:
```
F:\Git\suji-asr\vendor\ffmpeg-master-latest-win64-lgpl\bin\ffmpeg.exe -y ^
  -f lavfi -i "color=c=black:s=320x240:r=25" ^
  -i "F:\Git\suji-asr\models\sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25\test_wavs\0.wav" ^
  -c:v mpeg4 -c:a aac -shortest ^
  F:\Git\suji-asr\build\test_video.mp4
```

- [ ] **Step 2: Run selftest-gui and capture output**

```
build\Release\suji_gui.exe --selftest-gui F:\Git\suji-asr\build\test_video.mp4
```

Expected output pattern (each second until done):
```
row0=处理中  status=处理中 0/1  0.0 倍速
row0=处理中  status=处理中 0/1  2.3 倍速
...
row0=完成  status=完成: 1 成功  0 失败  0 取消  ...秒
```

PASS criterion: at least one line shows `row0=处理中` before the final `row0=完成` line. A line with `row0=待处理` during transcription = FAIL (bug not fixed).

- [ ] **Step 3: If selftest passes, do the final combined commit**

```
git add -A
git commit -m "fix: live progress during transcription + 处理中 row status (GUI no longer looks stuck at 待处理)"
```

---

### Task 5: Write bug report

**Files:**
- Create: `F:\Git\suji-asr\.superpowers\sdd\bug2-progress-report.md`

- [ ] **Step 1: Write the report**

Document:
1. Root cause (cb only in finalize loop)
2. The live-cb change in `batch_engine.cpp` (throttled 150 ms)
3. The GUI changes (`onWorkerStarted`, `onWorkerProgress`, `onWorkerFinished`)
4. Test result (full suite count, new test case)
5. The `--selftest-gui` output showing "处理中" mid-run
6. Any concerns or follow-up items

- [ ] **Step 2: Commit report**

```
git add .superpowers/sdd/bug2-progress-report.md
git commit -m "docs: bug2 live-progress fix report"
```
