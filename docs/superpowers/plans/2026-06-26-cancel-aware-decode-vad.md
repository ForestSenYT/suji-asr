# Cancel-Aware Decode & VAD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `decode_to_pcm` and `Vad::segment` cancel-aware so clicking Cancel in the GUI responds within ~1 s instead of waiting for full file decode+VAD (which can take 15-30 s on a 30-60 min lecture).

**Architecture:** Add an optional `const CancelToken* cancel = nullptr` trailing parameter to both blocking functions. In `decode_to_pcm` the ReadFile loop checks the token each iteration and calls `TerminateProcess` on cancel. In `Vad::segment` the window loop checks every 64 windows. `batch_engine.cpp` forwards the existing `cancel` pointer to both calls; `engine_worker.cpp` adds a cancel check between duration probes. Default args keep all existing call sites (tests, CLI, pipeline) compiling unchanged.

**Tech Stack:** C++17, MSVC /utf-8 /W4, Win32 API (TerminateProcess), doctest, CMake 4.3.3, Qt6 (for GUI integration only).

## Global Constraints

- C++17, MSVC, `/utf-8 /W4` — zero new warnings permitted.
- All function signatures use `const CancelToken* cancel = nullptr` trailing default — no existing call sites may be broken.
- `decode_to_pcm` cancel path: `TerminateProcess`, then close pipe_read, pi.hProcess, pi.hThread (and nul_handle if still open), set `err = "cancelled"`, return `false`.
- `Vad::segment` cancel check every 64 windows (not every sample); returns partial `out` — downstream `batch_engine` marks file cancelled.
- Build command: `F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release`
- Test binary: `build\Release\suji_tests.exe` — baseline 44 cases, all must stay green.
- End-to-end: `build\Release\suji_gui.exe --selftest-gui F:\Git\suji-asr\build\long15.wav 3` — row0 must reach "取消" within ~1 s of the cancel click.

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `src/core/media_decode.h` | Modify | Add `cancel` param to `decode_to_pcm` declaration |
| `src/core/media_decode.cpp` | Modify | Check cancel in ReadFile loop; TerminateProcess + cleanup on cancel |
| `src/core/vad.h` | Modify | Add `cancel` param to `segment` declaration |
| `src/core/vad.cpp` | Modify | Check cancel every 64 windows in segment loop |
| `src/core/batch_engine.cpp` | Modify | Pass `cancel` to `decode_to_pcm` and `vad.segment`; add post-decode cancel check |
| `src/gui/engine_worker.cpp` | Modify | Check `cancel_` between probe_duration_seconds calls |
| `tests/integration/test_media_decode.cpp` | Modify | Add cancel-aware decode test (pre-cancelled token, long wav, assert fast + false) |

---

### Task 1: Cancel-aware `decode_to_pcm`

**Files:**
- Modify: `src/core/media_decode.h`
- Modify: `src/core/media_decode.cpp`
- Test: `tests/integration/test_media_decode.cpp`

**Interfaces:**
- Produces: `bool decode_to_pcm(const std::string& ffmpeg, const std::string& input, AudioBuffer& out, std::string& err, const CancelToken* cancel = nullptr);`
  - Returns `false` + `err = "cancelled"` immediately when `cancel != nullptr && cancel->is_cancelled()` is detected in the ReadFile loop.
  - Kills child with `TerminateProcess(pi.hProcess, 1)` before closing handles.
  - All existing callers (no 5th arg) compile unchanged — default arg is `nullptr`, behaviour identical to today.

- [ ] **Step 1.1: Write the failing test in `tests/integration/test_media_decode.cpp`**

  Add a new `TEST_CASE` at the end of the file (after the existing Chinese-filename test). The test creates a 120-second WAV in the build temp area using the bundled ffmpeg with `-stream_loop`, then calls `decode_to_pcm` with a pre-cancelled token and asserts it returns `false` quickly.

  The key additions:

  ```cpp
  #include "core/cancel.h"
  #include <chrono>
  // (add after existing #includes at top of test_media_decode.cpp)
  ```

  ```cpp
  TEST_CASE("decode_to_pcm respects pre-cancelled token" * doctest::timeout(10)) {
      // Build a ~120-second WAV by looping the short test wav.
      // We stream_loop inside ffmpeg so we don't need a huge source.
      std::string long_wav;
      {
          wchar_t tmp_buf[MAX_PATH];
          GetTempPathW(MAX_PATH, tmp_buf);
          std::wstring tmp_dir_w(tmp_buf);
          if (!tmp_dir_w.empty() && tmp_dir_w.back() == L'\\') tmp_dir_w.pop_back();
          std::wstring long_w = tmp_dir_w + L"\\suji_cancel_test_long.wav";
          // Convert back to UTF-8 for std::string API
          int n = WideCharToMultiByte(CP_UTF8, 0, long_w.c_str(), -1, nullptr, 0, nullptr, nullptr);
          long_wav.resize(n - 1);
          WideCharToMultiByte(CP_UTF8, 0, long_w.c_str(), -1, long_wav.data(), n, nullptr, nullptr);

          // Create the long file once using ffmpeg -stream_loop
          // ffmpeg exits quickly — we just need a >30-second wav to exist on disk
          std::wstring src_w = to_wide(wav0());
          std::wstring ffmpeg_w = to_wide(ffmpeg_exe());
          std::wstring cmd = L"\"" + ffmpeg_w + L"\" -y -stream_loop 15 -i \""
                           + src_w + L"\" -t 120 -ar 16000 -ac 1 \""
                           + long_w + L"\"";

          STARTUPINFOW si{}; si.cb = sizeof(si);
          PROCESS_INFORMATION pi{};
          // Suppress output: redirect stdout/stderr to NUL
          SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
          HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &sa, OPEN_EXISTING, 0, nullptr);
          si.dwFlags = STARTF_USESTDHANDLES;
          si.hStdInput = nullptr; si.hStdOutput = nul; si.hStdError = nul;
          BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                                   TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
          if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
          REQUIRE_MESSAGE(ok, "ffmpeg -stream_loop failed to create long test wav");
          WaitForSingleObject(pi.hProcess, 30000);  // give ffmpeg up to 30 s
          CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
          REQUIRE(std::filesystem::exists(long_wav));
      }

      // Pre-cancel the token before calling decode_to_pcm
      suji::CancelToken tok;
      tok.cancel();

      suji::AudioBuffer ab; std::string err;
      auto t0 = std::chrono::steady_clock::now();
      bool result = suji::decode_to_pcm(ffmpeg_exe(), long_wav, ab, err, &tok);
      double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

      CHECK_FALSE(result);                    // must fail
      CHECK(err == "cancelled");              // error string must say "cancelled"
      CHECK(elapsed < 2.0);                   // must return in under 2 s
      // samples may be partially filled or empty — both are fine
      // (TerminateProcess kills ffmpeg mid-stream; whatever was buffered is irrelevant)

      // Cleanup
      std::filesystem::remove(long_wav);

      // Regression: NULL cancel (old signature) still works
      suji::AudioBuffer ab2; std::string err2;
      // Use the short wav (fast) with no cancel token — must return true
      bool ok2 = suji::decode_to_pcm(ffmpeg_exe(), wav0(), ab2, err2);
      CHECK(ok2);
      CHECK(ab2.samples.size() > 0);
  }
  ```

- [ ] **Step 1.2: Run test to see it fail (compile error expected)**

  ```powershell
  F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release 2>&1 | Select-String -Pattern "error|warning" | Select-Object -First 30
  ```

  Expected: compile error — `decode_to_pcm` does not yet accept a 5th argument, and `suji::CancelToken` is not included in `test_media_decode.cpp`.

- [ ] **Step 1.3: Update `src/core/media_decode.h`**

  Replace the existing declaration:

  ```cpp
  // BEFORE
  bool decode_to_pcm(const std::string& ffmpeg, const std::string& input, AudioBuffer& out, std::string& err);
  ```

  With:

  ```cpp
  // AFTER
  #pragma once
  #include "core/types.h"
  #include "core/cancel.h"
  #include <string>
  namespace suji {
  bool decode_to_pcm(const std::string& ffmpeg, const std::string& input,
                     AudioBuffer& out, std::string& err,
                     const CancelToken* cancel = nullptr);

  /// Run ffprobe to get the total duration of the media file.
  /// Returns duration in seconds, or -1.0 on any failure. Never throws.
  double probe_duration_seconds(const std::string& ffprobe, const std::string& input);
  }
  ```

- [ ] **Step 1.4: Update `src/core/media_decode.cpp` — add cancel to signature and ReadFile loop**

  Change the function signature line (line 19-20):

  ```cpp
  bool decode_to_pcm(const std::string& ffmpeg, const std::string& input,
                     AudioBuffer& out, std::string& err,
                     const CancelToken* cancel) {
  ```

  Add `#include "core/cancel.h"` after line 3 (`#include "core/media_decode.h"`).

  The ReadFile loop (currently lines 107-110) becomes:

  ```cpp
  while (ReadFile(pipe_read, chunk.data(), static_cast<DWORD>(chunk.size()), &bytes_read, nullptr)
         && bytes_read > 0) {
      if (cancel && cancel->is_cancelled()) {
          // Kill ffmpeg immediately — do not wait for EOF
          TerminateProcess(pi.hProcess, 1);
          CloseHandle(pipe_read);
          WaitForSingleObject(pi.hProcess, 5000);
          CloseHandle(pi.hProcess);
          CloseHandle(pi.hThread);
          out.samples.clear();
          err = "cancelled";
          return false;
      }
      raw_bytes.insert(raw_bytes.end(), chunk.begin(), chunk.begin() + bytes_read);
  }
  ```

  > Note: `WaitForSingleObject` after `TerminateProcess` is needed because `pi.hProcess` must be closed cleanly. 5000 ms upper bound is safe — TerminateProcess is nearly instant. `nul_handle` is already closed before this point (line 90 in original), so no double-close.

- [ ] **Step 1.5: Build and run the new test**

  ```powershell
  F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
  ```

  Expected: clean build, zero errors.

  ```powershell
  build\Release\suji_tests.exe -tc="decode_to_pcm respects pre-cancelled token" -v
  ```

  Expected: `TEST SUITE: ... PASSED` in under 10 s.

- [ ] **Step 1.6: Run full suite**

  ```powershell
  build\Release\suji_tests.exe
  ```

  Expected: all tests pass (≥ 44 cases now + 1 new = ≥ 45). Zero failures.

- [ ] **Step 1.7: Commit**

  ```powershell
  git add src/core/media_decode.h src/core/media_decode.cpp tests/integration/test_media_decode.cpp
  git commit -m "fix: cancel-aware decode_to_pcm (TerminateProcess on cancel)"
  ```

---

### Task 2: Cancel-aware `Vad::segment`

**Files:**
- Modify: `src/core/vad.h`
- Modify: `src/core/vad.cpp`

**Interfaces:**
- Consumes: `CancelToken` from `src/core/cancel.h`
- Produces: `std::vector<SpeechSeg> segment(const AudioBuffer& audio, const CancelToken* cancel = nullptr);`
  - Checks `cancel->is_cancelled()` every 64 windows (i.e., when `(i / window_) % 64 == 0`).
  - On cancel: breaks immediately, flushes VAD, appends any remaining queued segments, returns partial `out`.
  - Returning partial results is intentional — `batch_engine` marks the file as cancelled regardless.

- [ ] **Step 2.1: Update `src/core/vad.h`**

  Change:
  ```cpp
  // BEFORE
  std::vector<SpeechSeg> segment(const AudioBuffer& audio);
  ```

  To:
  ```cpp
  // AFTER
  #pragma once
  #include "core/types.h"
  #include "core/config.h"
  #include "core/cancel.h"
  #include <vector>
  struct SherpaOnnxVoiceActivityDetector;
  namespace suji {
  class Vad {
  public:
    explicit Vad(const EngineConfig& cfg);
    ~Vad();
    Vad(const Vad&) = delete; Vad& operator=(const Vad&) = delete;
    bool ok() const { return vad_ != nullptr; }
    std::vector<SpeechSeg> segment(const AudioBuffer& audio,
                                   const CancelToken* cancel = nullptr);
  private:
    const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
    int window_ = 512;
  };
  }
  ```

- [ ] **Step 2.2: Update `src/core/vad.cpp` — add cancel check every 64 windows**

  Change the function signature:

  ```cpp
  std::vector<SpeechSeg> Vad::segment(const AudioBuffer& audio,
                                       const CancelToken* cancel) {
  ```

  Add `#include "core/cancel.h"` after `#include "core/vad.h"`.

  The main loop (currently lines 23-30 in vad.cpp) becomes:

  ```cpp
  int64_t window_count = 0;
  for (int64_t i = 0; i + window_ <= total; i += window_, ++window_count) {
      // Check cancel every 64 windows (~32768 samples = ~2 s at 16 kHz)
      if (cancel && (window_count % 64 == 0) && cancel->is_cancelled()) break;
      SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, p + i, window_);
      while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
          const SherpaOnnxSpeechSegment* s = SherpaOnnxVoiceActivityDetectorFront(vad_);
          SpeechSeg seg; seg.start_sample = s->start; seg.samples.assign(s->samples, s->samples + s->n);
          out.push_back(std::move(seg));
          SherpaOnnxDestroySpeechSegment(s);
          SherpaOnnxVoiceActivityDetectorPop(vad_);
      }
  }
  ```

  The flush block after the loop remains unchanged — it handles whatever is already queued in the VAD state.

- [ ] **Step 2.3: Build**

  ```powershell
  F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
  ```

  Expected: clean build, zero errors/warnings.

- [ ] **Step 2.4: Run full suite**

  ```powershell
  build\Release\suji_tests.exe
  ```

  Expected: all tests pass (≥ 45 cases). The existing `vad yields segments` test calls `segment(ab)` with no second arg — the default `nullptr` keeps it working identically.

- [ ] **Step 2.5: Commit**

  ```powershell
  git add src/core/vad.h src/core/vad.cpp
  git commit -m "fix: cancel-aware VAD segment (checks token every 64 windows)"
  ```

---

### Task 3: Wire cancel through `batch_engine.cpp` and `engine_worker.cpp`

**Files:**
- Modify: `src/core/batch_engine.cpp` (lines 46, 52)
- Modify: `src/gui/engine_worker.cpp` (lines 118-121)

**Interfaces:**
- Consumes:
  - `decode_to_pcm(cfg.ffmpeg_path, inputs[fi], ab, err, cancel)` — 5th arg wired
  - `vad.segment(ab, cancel)` — 2nd arg wired
  - `cancel_.is_cancelled()` in `EngineWorker::run()`

- [ ] **Step 3.1: Update `src/core/batch_engine.cpp` — pass cancel to decode and VAD, add post-decode check**

  Current producer lambda body (lines 43-54 of batch_engine.cpp):

  ```cpp
  while((fi=next_file.fetch_add(1)) < (size_t)N){
      if(cancel && cancel->is_cancelled()) break;
      AudioBuffer ab; std::string err;
      if(!decode_to_pcm(cfg.ffmpeg_path, inputs[fi], ab, err)){
          { std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="decode: "+err; }
          continue;
      }
      Vad vad(cfg);
      if(!vad.ok()){ std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="VAD init"; continue; }
      auto segs = vad.segment(ab);
      for(auto& s : segs){ SegTask st; st.file_id=(int)fi; st.start_sample=s.start_sample; st.samples=std::move(s.samples); queue.push(std::move(st)); }
  }
  ```

  Replace with:

  ```cpp
  while((fi=next_file.fetch_add(1)) < (size_t)N){
      if(cancel && cancel->is_cancelled()) break;
      AudioBuffer ab; std::string err;
      if(!decode_to_pcm(cfg.ffmpeg_path, inputs[fi], ab, err, cancel)){
          std::lock_guard<std::mutex> lk(err_mu);
          results[fi].ok=false;
          results[fi].err = (err == "cancelled") ? "cancelled" : "decode: " + err;
          continue;
      }
      // Re-check after decode: a just-decoded file should not enter VAD when already cancelled.
      if(cancel && cancel->is_cancelled()){
          std::lock_guard<std::mutex> lk(err_mu);
          results[fi].ok=false; results[fi].err="cancelled";
          continue;
      }
      Vad vad(cfg);
      if(!vad.ok()){ std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="VAD init"; continue; }
      auto segs = vad.segment(ab, cancel);
      for(auto& s : segs){ SegTask st; st.file_id=(int)fi; st.start_sample=s.start_sample; st.samples=std::move(s.samples); queue.push(std::move(st)); }
  }
  ```

- [ ] **Step 3.2: Update `src/gui/engine_worker.cpp` — cancel check in probe loop**

  Current probe loop (lines 117-121):

  ```cpp
  double totalAudio = 0.0;
  for (const std::string& f : vec) {
      double d = probe_duration_seconds(ffprobe_path(), f);
      if (d > 0.0) totalAudio += d;
  }
  ```

  Replace with:

  ```cpp
  double totalAudio = 0.0;
  for (const std::string& f : vec) {
      if (cancel_.is_cancelled()) break;
      double d = probe_duration_seconds(ffprobe_path(), f);
      if (d > 0.0) totalAudio += d;
  }
  ```

- [ ] **Step 3.3: Build**

  ```powershell
  F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe --build build --config Release
  ```

  Expected: clean build, zero errors.

- [ ] **Step 3.4: Run full suite**

  ```powershell
  build\Release\suji_tests.exe
  ```

  Expected: all tests pass (≥ 45 cases). The existing `cancel returns promptly without deadlock` test exercises this path — it should still pass.

- [ ] **Step 3.5: Commit**

  ```powershell
  git add src/core/batch_engine.cpp src/gui/engine_worker.cpp
  git commit -m "fix: cancel-aware VAD + wire cancel through batch_engine/probe"
  ```

---

### Task 4: End-to-end verification and report

**Files:**
- Read: `F:\Git\suji-asr\build\long15.wav` (create if missing)
- Write: `F:\Git\suji-asr\.superpowers\sdd\bug-cancel-report.md`

- [ ] **Step 4.1: Ensure `long15.wav` exists (create if missing)**

  ```powershell
  if (-not (Test-Path "F:\Git\suji-asr\build\long15.wav")) {
      $ffmpeg = "F:\Git\suji-asr\vendor\ffmpeg-master-latest-win64-lgpl\bin\ffmpeg.exe"
      $src    = "F:\Git\suji-asr\models\sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25\test_wavs\0.wav"
      & $ffmpeg -y -stream_loop 300 -i $src -t 900 -ar 16000 -ac 1 "F:\Git\suji-asr\build\long15.wav"
  }
  ```

  Expected: `build\long15.wav` exists. (This is a 15-minute / 900-second WAV.)

- [ ] **Step 4.2: Run `--selftest-gui` and capture cancel latency**

  ```powershell
  build\Release\suji_gui.exe --selftest-gui F:\Git\suji-asr\build\long15.wav 3 2>&1
  ```

  Expected output includes a line like:
  ```
  >>> Cancel clicked now
  ...
  [T+~1.0s] row0 status: 取消
  ```

  The row must reach "取消" within ~1 s of `>>> Cancel clicked now` (previously ~3.5 s).

- [ ] **Step 4.3: Create `.superpowers/sdd/` directory if missing**

  ```powershell
  New-Item -ItemType Directory -Force "F:\Git\suji-asr\.superpowers\sdd" | Out-Null
  ```

- [ ] **Step 4.4: Write `bug-cancel-report.md`**

  Write to `F:\Git\suji-asr\.superpowers\sdd\bug-cancel-report.md`:

  ```markdown
  # Cancel-Hang Bug Report

  ## Root Cause
  `batch_engine.cpp` checked `cancel->is_cancelled()` only at the TOP of the per-file loop (before `decode_to_pcm`).
  For a single long file the one producer thread was blocked inside two sequential calls that had no cancel awareness:
  1. `decode_to_pcm(...)` — blocking `ReadFile` loop reading the full file's PCM from ffmpeg stdout.
  2. `vad.segment(ab)` — looping over all decoded samples in 512-sample windows.
  Cancel could not take effect until both completed, causing 3.5–30 s freezes depending on file length.
  Additionally `engine_worker.cpp` probed durations with no cancel check between files.

  ## Changes Made

  | File | Change |
  |------|--------|
  | `src/core/media_decode.h` | Added `const CancelToken* cancel = nullptr` param to `decode_to_pcm` |
  | `src/core/media_decode.cpp` | ReadFile loop checks cancel each iteration; `TerminateProcess` + handle cleanup on cancel |
  | `src/core/vad.h` | Added `const CancelToken* cancel = nullptr` param to `Vad::segment` |
  | `src/core/vad.cpp` | Window loop checks cancel every 64 windows (~2 s of audio) |
  | `src/core/batch_engine.cpp` | Passes `cancel` to `decode_to_pcm` and `vad.segment`; post-decode cancel check added |
  | `src/gui/engine_worker.cpp` | Duration probe loop checks `cancel_` between files |
  | `tests/integration/test_media_decode.cpp` | New test: pre-cancelled token on 120-s wav returns `false`/`"cancelled"` in < 2 s |

  ## Test Suite
  All [TOTAL] cases pass (was 44 before this fix, now [TOTAL]).

  ## Cancel-Latency Evidence (--selftest-gui)

  ### Before fix (estimated / reproduced)
  Row0 reached "取消" ~3.5 s after cancel click on 15-min file.

  ### After fix
  [PASTE OUTPUT FROM: build\Release\suji_gui.exe --selftest-gui F:\Git\suji-asr\build\long15.wav 3]

  Row0 reached "取消" within ~1 s of the cancel click.

  ## Commits
  [PASTE COMMIT SHAS AND SUBJECTS HERE]
  ```

  > Note: Fill in `[TOTAL]`, the selftest output, and commit SHAs as the final step after running verification.

- [ ] **Step 4.5: Final full suite count confirmation**

  ```powershell
  build\Release\suji_tests.exe 2>&1 | Select-String -Pattern "test cases|passed|failed"
  ```

  Expected: e.g. `46 test cases: 46 passed` (45 prior + 1 new cancel test).

---

## Self-Review

**Spec coverage check:**

1. `decode_to_pcm` cancel param + TerminateProcess — Task 1. ✓
2. `vad.segment` cancel param + 64-window check — Task 2. ✓
3. `batch_engine.cpp` wiring (decode + VAD + post-decode check) — Task 3. ✓
4. `engine_worker.cpp` probe loop cancel — Task 3. ✓
5. TDD test for cancel-aware decode (pre-cancelled, long wav, < 2 s, assert false + "cancelled", + regression) — Task 1. ✓
6. Build verify + `--selftest-gui` latency — Task 4. ✓
7. Commit per file-group — Tasks 1, 2, 3. ✓
8. Report to `.superpowers/sdd/bug-cancel-report.md` — Task 4. ✓

**Placeholder scan:** No TBD/TODO/placeholder text in code steps. All code is complete.

**Type consistency:**
- `CancelToken*` — defined in `src/core/cancel.h`; used consistently as `const CancelToken*` everywhere.
- `decode_to_pcm` 5th param named `cancel` in both `.h` and `.cpp`. ✓
- `segment` 2nd param named `cancel` in both `.h` and `.cpp`. ✓
- `batch_engine.cpp` passes `cancel` (the `CancelToken*` parameter of `transcribe_batch_files`). ✓
- `engine_worker.cpp` uses `cancel_` (the member field of `EngineWorker`). ✓
