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

int main(int argc, char** argv) {
  if (argc < 2) {
    std::puts("usage: suji_batch <dir|files...> [-o out_dir] [--provider auto|cpu|cuda] [--batch N] [--in-flight N] [--cuda-dll-dir <path>] [--resume|--no-resume]");
    return 2;
  }

  EngineConfig c;
  std::string mdl = models_dir();
  std::string m = mdl + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path = ffmpeg_path();
  c.asr_model   = m + "model.int8.onnx";
  c.tokens      = m + "tokens.txt";
  c.vad_model   = mdl + "/silero_vad.onnx";
  c.punct_model = mdl + "/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";

  std::string out_dir   = ".";
  std::string prov      = "auto";
  int         fbatch    = 0;
  int         finflight = 0;
  std::string cuda_dll_dir;
  bool        resume    = true;
  std::vector<std::string> inputs;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if      (a == "-o"            && i + 1 < argc) out_dir      = argv[++i];
    else if (a == "--provider"    && i + 1 < argc) prov         = argv[++i];
    else if (a == "--batch"       && i + 1 < argc) fbatch       = atoi(argv[++i]);
    else if (a == "--in-flight"   && i + 1 < argc) finflight    = atoi(argv[++i]);
    else if (a == "--cuda-dll-dir"&& i + 1 < argc) cuda_dll_dir = argv[++i];
    else if (a == "--resume")     resume = true;
    else if (a == "--no-resume")  resume = false;
    else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
      log_err("unknown flag " + a);
      return 2;
    } else {
      fs::path p(a);
      if (fs::is_directory(p)) {
        for (auto& e : fs::directory_iterator(p))
          if (e.is_regular_file() && is_media(e.path()))
            inputs.push_back(e.path().string());
      } else {
        inputs.push_back(a);
      }
    }
  }

  if (inputs.empty()) { log_err("no input media files"); return 1; }

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

  if      (prov == "cpu")  tune.provider = Provider::Cpu;
  else if (prov == "cuda") tune.provider = Provider::Cuda;
  if (fbatch    > 0) tune.batch           = fbatch;
  if (finflight > 0) tune.in_flight_files = finflight;

  log_info("hw: gpu=" + std::string(hw.has_cuda_gpu ? hw.gpu_name : "none")
    + " cores=" + std::to_string(hw.cpu_threads)
    + " ramMB=" + std::to_string(hw.ram_free_mb));

  // Safe CUDA: only attempt CUDA when --cuda-dll-dir is explicitly provided,
  // or when --provider cuda was explicit without --cuda-dll-dir (do a probe first).
  // For --provider auto: if tune picked Cuda but no cuda_dll_dir, fall back to CPU
  // (CUDA DLLs are unlikely to be on PATH on this dev box, and a failed init may crash).
  if (tune.provider == Provider::Cuda) {
    if (!cuda_dll_dir.empty()) {
      // User supplied dll dir: set it and do a probe Asr to verify init
      c.cuda_dll_dir = cuda_dll_dir;
      EngineConfig probe_cfg = c;
      probe_cfg.provider = Provider::Cuda;
      Asr probe(probe_cfg);
      if (!probe.ok()) {
        log_err("CUDA unavailable (init failed), falling back to CPU");
        tune.provider  = Provider::Cpu;
        c.cuda_dll_dir = {};
      } else {
        log_info("CUDA probe ok, running on GPU");
      }
    } else {
      // No cuda_dll_dir supplied: skip CUDA attempt to avoid potential crash
      // (CUDA runtime DLLs not guaranteed on PATH; empirical behavior: may crash).
      log_err("CUDA selected but --cuda-dll-dir not provided; falling back to CPU for safety");
      tune.provider = Provider::Cpu;
    }
  }

  // Recompute CPU threads after any flip to CPU
  if (tune.provider == Provider::Cpu) tune.num_threads = std::max(4, hw.cpu_threads);
  // D8: recompute batch for CPU (GPU-decided value may be too large for CPU)
  if (tune.provider == Provider::Cpu) tune.batch = std::min(4, std::max(1, hw.cpu_threads / 4));

  // Log the FINAL tune AFTER fallback and recomputation
  log_info("tune: provider=" + std::string(provider_str(tune.provider))
    + " batch=" + std::to_string(tune.batch)
    + " in_flight=" + std::to_string(tune.in_flight_files)
    + " threads=" + std::to_string(tune.num_threads)
    + " files=" + std::to_string(todo.size()));

  // Apply final provider to engine config
  c.provider    = tune.provider;
  c.num_threads = tune.num_threads;

  auto t0 = std::chrono::steady_clock::now();
  double last_audio = 0.0;
  auto res = transcribe_batch_files(todo, c, tune, [&](const BatchProgress& b) {
    last_audio = b.audio_seconds_done;
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
      write_outputs(r.transcript, b, c, stem(r.input));
    } else {
      failc++;
      log_err("FAILED " + r.input + ": " + r.err);
    }
  }
  std::printf("\ndone: %d/%zu ok, skipped(resumed)=%d, failed=%d, wall=%.1fs, throughput=%.1fx realtime\n",
    okc, todo.size(), skipped, failc, wall, wall > 0 ? last_audio / wall : 0.0);
  return (okc > 0 || skipped > 0) ? 0 : 1;
}
