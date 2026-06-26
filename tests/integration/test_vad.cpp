#include "doctest/doctest.h"
#include "core/vad.h"
#include "core/media_decode.h"
#include "core/config.h"
#include "core/cancel.h"
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <windows.h>
using namespace suji;
static EngineConfig cfg(){ EngineConfig c; c.ffmpeg_path=SUJI_DEFAULT_FFMPEG;
  c.vad_model=std::string(SUJI_DEFAULT_MODELS_DIR)+"/silero_vad.onnx"; return c; }
TEST_CASE("vad yields segments" * doctest::timeout(60)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());
  auto segs = vad.segment(ab);
  REQUIRE(segs.size() >= 1);
  CHECK(segs[0].samples.size() > 0);
  CHECK(segs[0].start_sample >= 0);
}

// T11: one Vad is now built per producer thread and REUSED across files. This
// proves the reuse is safe: segmenting file B with a Vad that already segmented
// file A (reused) yields the SAME segments as a fresh Vad on file B. Vad::segment()
// calls Reset() internally, so prior-file LSTM state / queued segments never leak.
TEST_CASE("vad reuse across files matches fresh vad (T11)" * doctest::timeout(120)) {
  std::string w = std::string(SUJI_DEFAULT_MODELS_DIR)
                + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  AudioBuffer a0, a1; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path, w + "0.wav", a0, err));
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path, w + "1.wav", a1, err));

  // Reused detector: segment file 0, then file 1 on the SAME Vad.
  Vad reused(cfg()); REQUIRE(reused.ok());
  (void)reused.segment(a0);
  auto segs_reused = reused.segment(a1);

  // Fresh detector for file 1 (the per-file baseline).
  Vad fresh(cfg()); REQUIRE(fresh.ok());
  auto segs_fresh = fresh.segment(a1);

  REQUIRE(segs_reused.size() == segs_fresh.size());
  for (size_t i = 0; i < segs_fresh.size(); ++i) {
    CHECK(segs_reused[i].start_sample == segs_fresh[i].start_sample);
    CHECK(segs_reused[i].samples.size() == segs_fresh[i].samples.size());
  }
}

// P2: segment_stream emits the SAME segments (count + start_sample + sample count),
// in the same order, as segment() — since segment() is now built on segment_stream,
// this proves the streaming path and the collected path are one and the same.
TEST_CASE("vad segment_stream equals segment (P2)" * doctest::timeout(120)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());

  auto collected = vad.segment(ab);             // baseline (now built on segment_stream)
  REQUIRE(collected.size() >= 1);

  // Drive segment_stream directly and accumulate; callback must fire once per segment.
  std::vector<SpeechSeg> streamed;
  int callbacks = 0;
  vad.segment_stream(ab, [&](SpeechSeg&& s){ ++callbacks; streamed.push_back(std::move(s)); return true; });

  CHECK(callbacks == (int)collected.size());           // one callback per emitted segment
  REQUIRE(streamed.size() == collected.size());
  for (size_t i = 0; i < collected.size(); ++i) {
    CHECK(streamed[i].start_sample == collected[i].start_sample);
    CHECK(streamed[i].samples.size() == collected[i].samples.size());
  }
}

// P2: returning false from the callback stops emission early. With >=2 segments,
// stopping after the first means exactly one callback fires.
TEST_CASE("vad segment_stream early-stop on callback false (P2)" * doctest::timeout(120)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());
  REQUIRE(vad.segment(ab).size() >= 2);   // file has multiple segments

  int callbacks = 0;
  vad.segment_stream(ab, [&](SpeechSeg&&){ ++callbacks; return false; });  // stop after first
  CHECK(callbacks == 1);                  // emission stopped immediately on false
}

// P2: a cancelled token aborts segment_stream just like segment(); no callbacks
// after cancel (pre-cancelled token yields zero segments).
TEST_CASE("vad segment_stream honors cancel (P2)" * doctest::timeout(120)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());
  CancelToken cancel; cancel.cancel();    // already cancelled
  int callbacks = 0;
  vad.segment_stream(ab, [&](SpeechSeg&&){ ++callbacks; return true; }, &cancel);
  CHECK(callbacks == 0);                   // cancel checked at loop entry -> no emission
}

// P3 (streaming decode) — KEY CORRECTNESS: feeding the SAME audio to the incremental
// accept()/finish() API in DIFFERENT chunk sizes must yield byte-for-byte IDENTICAL
// segments (count + start_sample + sample count), and those must equal the old
// segment()/segment_stream() output. Window-aligned feeding buffers the remainder so the
// AcceptWaveform call sequence is independent of how the input is chopped up.
TEST_CASE("vad accept is chunk-invariant and equals segment (P3)" * doctest::timeout(180)) {
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path,
    std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(cfg()); REQUIRE(vad.ok());

  // Baseline: the whole-buffer collected path (segment() -> segment_stream()).
  auto baseline = vad.segment(ab);
  REQUIRE(baseline.size() >= 2);   // multiple segments make the comparison meaningful

  // Helper: drive accept() with a fixed chunk size, then finish(); collect segments.
  auto run_chunked = [&](int chunk) {
    std::vector<SpeechSeg> out;
    auto sink = [&](SpeechSeg&& s){ out.push_back(std::move(s)); return true; };
    vad.reset();
    const float* p = ab.samples.data();
    int total = (int)ab.samples.size();
    for (int i = 0; i < total; i += chunk) {
      int n = (i + chunk <= total) ? chunk : (total - i);
      REQUIRE(vad.accept(p + i, n, sink));   // no cancel -> never stops early
    }
    vad.finish(sink);
    return out;
  };

  // Compare a chunked run against the baseline (identical count + start + sample count).
  auto same_as_baseline = [&](const std::vector<SpeechSeg>& got){
    REQUIRE(got.size() == baseline.size());
    for (size_t i = 0; i < baseline.size(); ++i) {
      CHECK(got[i].start_sample == baseline[i].start_sample);
      CHECK(got[i].samples.size() == baseline[i].samples.size());
    }
  };

  // One big call (whole buffer), then progressively pathological chunk sizes:
  // tiny (< window), window-aligned, window+1 (forces leftover carry), an odd prime,
  // and a large multi-window chunk. ALL must match the baseline exactly.
  same_as_baseline(run_chunked((int)ab.samples.size()));  // single accept()
  same_as_baseline(run_chunked(1));                       // one sample at a time
  same_as_baseline(run_chunked(100));                     // < window_ (512)
  same_as_baseline(run_chunked(512));                     // window-aligned
  same_as_baseline(run_chunked(513));                     // window+1 (leftover carry)
  same_as_baseline(run_chunked(997));                     // odd prime (never window-aligned)
  same_as_baseline(run_chunked(40000));                   // large multi-window chunk
}

// P3 (streaming decode) — STREAMING EQUIVALENCE: decode_vad_stream on a real wav must
// emit the SAME segments (count + start_sample + sample count) as the two-step path
// decode_to_pcm + vad.segment on that same wav. This proves feeding ffmpeg's PCM
// incrementally yields identical segmentation to buffering the whole file first.
TEST_CASE("decode_vad_stream equals decode_to_pcm + segment (P3)" * doctest::timeout(180)) {
  std::string wav = std::string(SUJI_DEFAULT_MODELS_DIR)
                  + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav";

  // Two-step baseline.
  AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(cfg().ffmpeg_path, wav, ab, err));
  Vad vbase(cfg()); REQUIRE(vbase.ok());
  auto baseline = vbase.segment(ab);
  REQUIRE(baseline.size() >= 1);

  // Streaming path: same Vad reused is fine (decode_vad_stream calls reset() internally).
  Vad vstream(cfg()); REQUIRE(vstream.ok());
  std::vector<SpeechSeg> streamed; std::string serr;
  bool ok = decode_vad_stream(cfg().ffmpeg_path, wav, vstream,
    [&](SpeechSeg&& s){ streamed.push_back(std::move(s)); return true; }, serr);
  REQUIRE_MESSAGE(ok, serr);

  REQUIRE(streamed.size() == baseline.size());
  for (size_t i = 0; i < baseline.size(); ++i) {
    CHECK(streamed[i].start_sample == baseline[i].start_sample);
    CHECK(streamed[i].samples.size() == baseline[i].samples.size());
  }
}

// P3 — decode_vad_stream returns false + err for a missing input (spawn/no-audio path).
TEST_CASE("decode_vad_stream missing file fails" * doctest::timeout(30)) {
  Vad vad(cfg()); REQUIRE(vad.ok());
  std::string err;
  bool ok = decode_vad_stream(cfg().ffmpeg_path, "no_such_file_xyz.wav", vad,
    [&](SpeechSeg&&){ return true; }, err);
  CHECK_FALSE(ok);
  CHECK_FALSE(err.empty());
}

// P3 — a pre-cancelled token aborts decode_vad_stream quickly (TerminateProcess),
// returning false with err == "cancelled" in well under 2 s on a long file.
TEST_CASE("decode_vad_stream respects pre-cancelled token" * doctest::timeout(30)) {
  // Build a ~120s wav by looping the short test wav inside ffmpeg (same trick as the
  // media_decode cancel test).
  wchar_t tmp_buf[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp_buf);
  std::wstring tmp_dir_w(tmp_buf);
  if (!tmp_dir_w.empty() && tmp_dir_w.back() == L'\\') tmp_dir_w.pop_back();
  std::wstring long_w = tmp_dir_w + L"\\suji_stream_cancel_long.wav";
  int n = WideCharToMultiByte(CP_UTF8, 0, long_w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string long_wav(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, long_w.c_str(), -1, long_wav.data(), n, nullptr, nullptr);

  std::string src = std::string(SUJI_DEFAULT_MODELS_DIR)
                  + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav";
  {
    int sn = MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, nullptr, 0);
    std::wstring src_w(sn - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, src_w.data(), sn);
    int fn = MultiByteToWideChar(CP_UTF8, 0, cfg().ffmpeg_path.c_str(), -1, nullptr, 0);
    std::wstring ffmpeg_w(fn - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cfg().ffmpeg_path.c_str(), -1, ffmpeg_w.data(), fn);
    std::wstring cmd = L"\"" + ffmpeg_w + L"\" -y -stream_loop 15 -i \""
                     + src_w + L"\" -t 120 -ar 16000 -ac 1 \"" + long_w + L"\"";
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr; si.hStdOutput = nul; si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    BOOL spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    REQUIRE_MESSAGE(spawned, "ffmpeg -stream_loop failed to spawn");
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  }

  Vad vad(cfg()); REQUIRE(vad.ok());
  CancelToken tok; tok.cancel();   // pre-cancelled
  std::string err;
  auto t0 = std::chrono::steady_clock::now();
  bool result = decode_vad_stream(cfg().ffmpeg_path, long_wav, vad,
    [&](SpeechSeg&&){ return true; }, err, &tok);
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK_FALSE(result);
  CHECK(err == "cancelled");
  CHECK(elapsed < 2.0);
  DeleteFileW(long_w.c_str());
}

// P3 — MID-STREAM cancel: a token cancelled by a worker thread shortly after the stream
// starts must stop emission and return false ("cancelled"), not run to EOF.
TEST_CASE("decode_vad_stream mid-stream cancel stops" * doctest::timeout(30)) {
  // Build a ~60s wav so there is real streaming time to cancel within.
  wchar_t tmp_buf[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp_buf);
  std::wstring tmp_dir_w(tmp_buf);
  if (!tmp_dir_w.empty() && tmp_dir_w.back() == L'\\') tmp_dir_w.pop_back();
  std::wstring long_w = tmp_dir_w + L"\\suji_stream_midcancel_long.wav";
  int n = WideCharToMultiByte(CP_UTF8, 0, long_w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string long_wav(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, long_w.c_str(), -1, long_wav.data(), n, nullptr, nullptr);

  std::string src = std::string(SUJI_DEFAULT_MODELS_DIR)
                  + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav";
  {
    int sn = MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, nullptr, 0);
    std::wstring src_w(sn - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, src.c_str(), -1, src_w.data(), sn);
    int fn = MultiByteToWideChar(CP_UTF8, 0, cfg().ffmpeg_path.c_str(), -1, nullptr, 0);
    std::wstring ffmpeg_w(fn - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cfg().ffmpeg_path.c_str(), -1, ffmpeg_w.data(), fn);
    std::wstring cmd = L"\"" + ffmpeg_w + L"\" -y -stream_loop 8 -i \""
                     + src_w + L"\" -t 60 -ar 16000 -ac 1 \"" + long_w + L"\"";
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr; si.hStdOutput = nul; si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    BOOL spawned = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                                  TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    REQUIRE_MESSAGE(spawned, "ffmpeg -stream_loop failed to spawn");
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
  }

  Vad vad(cfg()); REQUIRE(vad.ok());
  CancelToken tok;
  // Cancel from another thread ~50ms after the stream starts (mid-stream).
  std::thread canceller([&]{ std::this_thread::sleep_for(std::chrono::milliseconds(50)); tok.cancel(); });
  std::string err;
  bool result = decode_vad_stream(cfg().ffmpeg_path, long_wav, vad,
    [&](SpeechSeg&&){ return true; }, err, &tok);
  canceller.join();
  CHECK_FALSE(result);
  CHECK(err == "cancelled");
  DeleteFileW(long_w.c_str());
}
