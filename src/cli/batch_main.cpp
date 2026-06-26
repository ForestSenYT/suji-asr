#include "core/hardware.h"
#include "core/batch_engine.h"
#include "core/output/writer_facade.h"
#include "core/config.h"
#include "core/paths.h"
#include "core/asr.h"
#include "core/log.h"
#include "core/resume.h"
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <filesystem>
#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#endif
using namespace suji;
namespace fs = std::filesystem;

static std::string stem(const std::string& p) {
  fs::path q(p);
  return q.stem().string();
}
static bool is_media(const fs::path& p) {
  static const char* ext[] = {
    ".mp4", ".mkv", ".mov", ".flv", ".avi", ".webm", ".ts",
    ".m4a", ".mp3", ".wav", ".flac", ".aac", ".ogg", ".opus"
  };
  std::string e = p.extension().string();
  for (auto& ch : e) ch = (char)tolower((unsigned char)ch);
  for (auto x : ext) if (e == x) return true;
  return false;
}

// Parse a positive integer flag (e.g. --batch N). Returns 0 on parse error or
// non-positive value; the caller skips the override when result==0.
static int parse_positive_int(const char* s) {
  try {
    int v = std::stoi(s);
    return v > 0 ? v : 0;
  } catch (...) {
    return 0;
  }
}

int main(int argc, char** argv) {
#ifdef _WIN32
  // Render UTF-8 log bytes (e.g. 解码/切分语音/Chinese filenames) correctly on a
  // Chinese console (codepage 936/GBK would otherwise show mojibake like 瑙ｇ爜).
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif
  if (argc < 2) {
    std::puts("usage: suji_batch <dir|files...> [-o out_dir] [--provider auto|cpu|cuda|hetero] [--batch N] [--cpu-batch N] [--gpu-batch N] [--cpu-threads N] [--in-flight N] [--cuda-dll-dir <path>] [--asr-encoder <path> --asr-decoder <path> [--asr-tokens <path>]] [--srt-line N] [--resume|--no-resume]");
    return 2;
  }

  EngineConfig c;
  { auto mp = default_model_paths();
    c.ffmpeg_path = ffmpeg_path();
    c.asr_model   = mp.asr_model;
    c.tokens      = mp.tokens;
    c.vad_model   = mp.vad_model;
    c.punct_model = mp.punct_model;
    c.rule_fsts   = discover_rule_fsts(); // auto-discover ITN FST/FAR; empty = ITN off
  }

  std::string out_dir        = ".";
  std::string prov           = "auto";
  int         fbatch         = 0;    // --batch (legacy alias for gpu-batch)
  int         fcpu_batch     = 0;    // --cpu-batch
  int         fgpu_batch     = 0;    // --gpu-batch
  int         fcpu_threads   = 0;    // --cpu-threads (overrides hetero cpu_asr_threads)
  int         finflight      = 0;
  std::string cuda_dll_dir_override;
  bool        resume         = true;
  int         fsrt_line      = 0;
  // P1: FireRedASR AED model overrides (encoder+decoder+tokens). When both
  // encoder and decoder are given, the engine uses the AED path (fire_red_asr).
  std::string aed_encoder, aed_decoder, aed_tokens;
  std::vector<std::string> inputs;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if      (a == "-o"             && i + 1 < argc) out_dir               = argv[++i];
    else if (a == "--provider"     && i + 1 < argc) prov                  = argv[++i];
    else if (a == "--batch"        && i + 1 < argc) {
      fbatch = parse_positive_int(argv[++i]);
      if (fbatch == 0) { log_err("--batch requires a positive integer, got: " + std::string(argv[i])); return 2; }
    }
    else if (a == "--cpu-batch"    && i + 1 < argc) {
      fcpu_batch = parse_positive_int(argv[++i]);
      if (fcpu_batch == 0) { log_err("--cpu-batch requires a positive integer, got: " + std::string(argv[i])); return 2; }
    }
    else if (a == "--gpu-batch"    && i + 1 < argc) {
      fgpu_batch = parse_positive_int(argv[++i]);
      if (fgpu_batch == 0) { log_err("--gpu-batch requires a positive integer, got: " + std::string(argv[i])); return 2; }
    }
    else if (a == "--cpu-threads"  && i + 1 < argc) {
      fcpu_threads = parse_positive_int(argv[++i]);
      if (fcpu_threads == 0) { log_err("--cpu-threads requires a positive integer, got: " + std::string(argv[i])); return 2; }
    }
    else if (a == "--in-flight"    && i + 1 < argc) {
      finflight = parse_positive_int(argv[++i]);
      if (finflight == 0) { log_err("--in-flight requires a positive integer, got: " + std::string(argv[i])); return 2; }
    }
    else if (a == "--cuda-dll-dir" && i + 1 < argc) cuda_dll_dir_override = argv[++i];
    else if (a == "--asr-encoder"  && i + 1 < argc) {
      aed_encoder = argv[++i];
      if (!fs::exists(aed_encoder)) { log_err("--asr-encoder file not found: " + aed_encoder); return 2; }
    }
    else if (a == "--asr-decoder"  && i + 1 < argc) {
      aed_decoder = argv[++i];
      if (!fs::exists(aed_decoder)) { log_err("--asr-decoder file not found: " + aed_decoder); return 2; }
    }
    else if (a == "--asr-tokens"   && i + 1 < argc) {
      aed_tokens = argv[++i];
      if (!fs::exists(aed_tokens)) { log_err("--asr-tokens file not found: " + aed_tokens); return 2; }
    }
    else if (a == "--srt-line"      && i + 1 < argc) {
      // 0 = no wrap (valid default); positive = max codepoints per line
      fsrt_line = parse_positive_int(argv[++i]);
    }
    else if (a == "--resume")     resume = true;
    else if (a == "--no-resume")  resume = false;
    else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      log_err("unknown flag " + a);
      return 2;
    } else {
      fs::path p(a);
      if (fs::is_directory(p)) {
        for (auto& e : fs::recursive_directory_iterator(p))
          if (e.is_regular_file() && is_media(e.path()))
            inputs.push_back(e.path().string());
      } else {
        inputs.push_back(a);
      }
    }
  }

  if (inputs.empty()) { log_err("no input media files"); return 1; }

  // P1: apply FireRedASR AED overrides. Encoder+decoder must be given together
  // (the engine only takes the AED path when BOTH are non-empty). Point tokens at
  // the AED model's tokens.txt; without it the CTC tokens would mismatch the AED vocab.
  if (aed_encoder.empty() != aed_decoder.empty()) {
    log_err("--asr-encoder and --asr-decoder must be given together");
    return 2;
  }
  if (!aed_encoder.empty()) {
    c.asr_encoder = aed_encoder;
    c.asr_decoder = aed_decoder;
    if (!aed_tokens.empty()) c.tokens = aed_tokens;
    log_info("asr: FireRedASR AED (encoder+decoder) selected");
  }
  if (!aed_tokens.empty() && aed_encoder.empty()) c.tokens = aed_tokens;

  // Resume partition: skip already-complete outputs
  std::vector<std::string> todo;
  int skipped = 0;
  for (auto& in : inputs) {
    std::string base = out_dir + "/" + stem(in);
    if (resume && transcript_complete(base, c)) {
      ++skipped;
      log_info("resume: skip (done) " + in);
    } else {
      todo.push_back(in);
    }
  }
  if (todo.empty()) {
    std::printf("nothing to do: %d already complete (resumed)\n", skipped);
    return 0;
  }

  // Hardware auto-tune
  HardwareInfo hw   = probe_hardware();
  AutoTune     tune = decide(hw, c);

  // --- Provider override ---
  if      (prov == "cpu")    tune.provider = Provider::Cpu;
  else if (prov == "cuda")   tune.provider = Provider::Cuda;
  else if (prov == "hetero") {
    // User forced hetero: validate hardware, then fill tunables.
    bool gpu_ok = hw.has_cuda_gpu && hw.gpu_free_mb >= 3000 && hw.cuda_runtime_available;
    bool cpu_ok = hw.cpu_threads >= 12;
    if (!(gpu_ok && cpu_ok)) {
      log_err("hetero unavailable (need CUDA GPU + >=12 cores), falling back");
      tune.provider = hw.has_cuda_gpu ? Provider::Cuda : Provider::Cpu;
    } else {
      // Pass the real (post-resume) file count so the thread split reclaims idle
      // producer cores when only a few files are queued.
      fill_hetero(tune, hw, static_cast<int>(todo.size()));
    }
  }
  // (prov == "auto") -> keep decide()'s choice as-is

  // --- Auto-prefer the fp16 FireRedASR AED model on GPU ---
  // When the fp16-AED model is present AND a working CUDA GPU is available AND the
  // user neither forced a CPU/hetero provider nor supplied their own --asr-encoder,
  // use fp16-AED on CUDA: it's ~9.2x faster than int8-CTC on GPU AND more accurate
  // (int8-CTC leaks "< sil >" markers + mis-recognizes words). This overrides the
  // int8 hetero default because fp16-AED-on-GPU alone beats int8-hetero. fp16 is
  // GPU-only (it crashes on the CPU EP), so the crash-safe CUDA-DLL fallback below
  // (empty cuda_dll_dir -> CPU) still protects against a missing CUDA runtime.
  {
    const bool user_forced_cpu_hetero = (prov == "cpu" || prov == "hetero");
    const bool user_gave_encoder      = !aed_encoder.empty();
    if (!user_forced_cpu_hetero && !user_gave_encoder &&
        hw.has_cuda_gpu && hw.cuda_runtime_available) {
      AedModel aed = discover_fp16_aed();
      if (aed.ok()) {
        tune.provider = Provider::Cuda;
        c.asr_encoder = aed.encoder;
        c.asr_decoder = aed.decoder;
        c.tokens      = aed.tokens;
        c.cuda_dll_dir = hw.cuda_dll_dir;
        log_info("using fp16 AED model on GPU (faster + more accurate than int8)");
      }
    }
  }

  // --- Resolve CUDA DLL directory for GPU (Cuda or Hetero) providers ---
  if (tune.provider == Provider::Cuda || tune.provider == Provider::Hetero) {
    c.cuda_dll_dir = (!cuda_dll_dir_override.empty() ? cuda_dll_dir_override : hw.cuda_dll_dir);
    if (c.cuda_dll_dir.empty()) {
      log_err("CUDA runtime not found, falling back to CPU");
      tune.provider = Provider::Cpu;
    }
  }

  // --- Apply per-engine batch overrides AFTER fill_hetero/decide so they win ---
  // --batch N is a legacy alias that sets gpu_batch (and tune.batch) for backward compat.
  if (fbatch    > 0) { tune.batch = fbatch; tune.gpu_batch = fbatch; }
  if (fgpu_batch > 0) { tune.gpu_batch = fgpu_batch; tune.batch = fgpu_batch; }
  if (fcpu_batch > 0)  tune.cpu_batch = fcpu_batch;
  // --cpu-threads overrides the hetero CPU-recognizer thread count (and its legacy
  // num_threads mirror). Lets the user reclaim idle cores during the long ASR phase.
  if (fcpu_threads > 0) { tune.cpu_asr_threads = fcpu_threads; tune.num_threads = fcpu_threads; }
  if (finflight > 0)   tune.in_flight_files = finflight;

  // --- Recompute CPU threads/batch after any flip to CPU ---
  if (tune.provider == Provider::Cpu) {
    tune.num_threads = std::max(4, hw.cpu_threads);
    // D8: recompute batch for CPU (GPU-decided value may be too large for CPU)
    tune.batch = std::min(4, std::max(1, hw.cpu_threads / 4));
  }

  log_info("hw: gpu=" + std::string(hw.has_cuda_gpu ? hw.gpu_name : "none")
    + " cores=" + std::to_string(hw.cpu_threads)
    + " ramMB=" + std::to_string(hw.ram_free_mb));

  // Log the FINAL tune AFTER fallback and recomputation
  if (tune.provider == Provider::Hetero) {
    log_info("tune: provider=hetero"
      " cpu_batch=" + std::to_string(tune.cpu_batch) +
      " gpu_batch=" + std::to_string(tune.gpu_batch) +
      " cpu_asr_threads=" + std::to_string(tune.cpu_asr_threads) +
      " in_flight=" + std::to_string(tune.in_flight_files) +
      " files=" + std::to_string(todo.size()));
  } else {
    log_info("tune: provider=" + std::string(provider_str(tune.provider))
      + " batch=" + std::to_string(tune.batch)
      + " in_flight=" + std::to_string(tune.in_flight_files)
      + " threads=" + std::to_string(tune.num_threads)
      + " files=" + std::to_string(todo.size()));
  }

  // Apply final provider to engine config
  c.provider    = tune.provider;
  c.num_threads = tune.num_threads;
  // G5: SRT/VTT line-wrap width (0 = no wrap, default)
  if (fsrt_line > 0) c.srt_max_chars_per_line = fsrt_line;

  auto t0 = std::chrono::steady_clock::now();
  double last_audio = 0.0;
  double total_decoded = 0.0;   // G13: full decoded audio duration (incl. silence) for throughput
  auto res = transcribe_batch_files(todo, c, tune, [&](const BatchProgress& b) {
    last_audio = b.audio_seconds_done;
    total_decoded = b.total_audio_decoded;
    double el  = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double eta = b.files_done > 0
      ? el * static_cast<double>(b.files_total - b.files_done) / static_cast<double>(b.files_done)
      : 0.0;
    std::fprintf(stderr, "\r[%d/%d] %.0fs audio, ETA %dm%02ds   ",
      b.files_done, b.files_total, b.audio_seconds_done,
      static_cast<int>(eta) / 60, static_cast<int>(eta) % 60);
  });
  double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  // Write outputs + tally
  fs::create_directories(out_dir);
  int okc = 0, failc = 0;
  std::set<std::string> used_bases;
  for (auto& r : res) {
    if (r.ok) {
      okc++;
      std::string base = out_dir + "/" + stem(r.input);
      std::string b = base; int n = 2;
      while (used_bases.count(b)) { b = base + "_" + std::to_string(n++); }
      used_bases.insert(b);
      if (b != base) log_err("output stem collision for '" + r.input + "' -> writing as " + b);
      if (!write_outputs(r.transcript, b, c, stem(r.input))) {
        log_err("write failed: " + b);
        okc--;   // was pre-incremented; undo it
        failc++;
      }
    } else {
      failc++;
      log_err("FAILED " + r.input + ": " + r.err);
    }
  }
  // G13: report throughput on the FULL decoded audio duration (incl. silence) so it
  // reflects true aggregate audio-hours/wall-hours, not just VAD-speech seconds.
  // Fall back to last_audio (speech) only if the decoded total is unavailable.
  double audio_for_tput = total_decoded > 0.0 ? total_decoded : last_audio;
  std::printf("\ndone: %d/%zu ok, skipped(resumed)=%d, failed=%d, wall=%.1fs, audio=%.0fs, throughput=%.1fx realtime\n",
    okc, todo.size(), skipped, failc, wall, audio_for_tput, wall > 0 ? audio_for_tput / wall : 0.0);
  return (okc > 0 || skipped > 0) ? 0 : 1;
}
