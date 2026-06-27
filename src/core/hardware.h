#pragma once
#include "core/config.h"
#include <string>
namespace suji {
struct HardwareInfo {
  bool has_cuda_gpu = false;
  std::string gpu_name;
  int gpu_free_mb = 0;
  int gpu_total_mb = 0;
  int cpu_threads = 1;
  int ram_total_mb = 0;
  int ram_free_mb = 0;
  bool cuda_runtime_available = false;
  std::string cuda_dll_dir;
};
struct AutoTune {
  Provider provider       = Provider::Cpu;
  int      batch          = 1;
  int      in_flight_files = 1;
  int      num_threads    = 1;
  // Data-parallel CPU consumers: K independent CPU recognizers (each its own Asr
  // handle + num_threads), all draining the ONE shared queue, results merged per
  // file at finalize. 1 = the single-consumer path (no behaviour change). >1 raises
  // AGGREGATE multi-file throughput for single-stream-bound models (Qwen3's 721M
  // autoregressive int8 decoder runs batch=1 serial, so one recognizer can't
  // saturate the cores). Set per-consumer num_threads ~= total_logical / K.
  int      cpu_consumers  = 1;
  // hetero per-engine fields
  bool     hetero         = false;
  int      cpu_batch      = 1;
  int      gpu_batch      = 8;
  int      cpu_asr_threads = 4;
};
HardwareInfo probe_hardware(const std::string& nvidia_smi = "nvidia-smi");
AutoTune decide(const HardwareInfo& hw, const EngineConfig& cfg);
// Reusable helper: fill t with the Hetero-mode tunable values derived from hw.
// Sets t.hetero=true, t.provider=Hetero, t.in_flight_files, t.cpu_asr_threads,
// t.cpu_batch, t.gpu_batch, t.num_threads, t.batch.
//
// FILE-COUNT-AWARE thread split (reclaims idle producer cores): producers run only
// during decode+VAD, which is SHORT vs the long ASR phase. P producer slots are
// reserved, but only min(P, num_files) of them ever run -- with few files the rest
// sit idle. fill_hetero gives the CPU recognizer the cores those idle producers
// would otherwise reserve, capped at the BENCHMARKED optimum C-P-gpu_feed (an
// empirical bench10.wav sweep on C=16/RTX2080 showed cpu_asr_threads above C-P-1
// REGRESSES single-file throughput -- sublinear ONNX intra-op scaling + contention
// with the GPU host thread). See fill_hetero() in hardware.cpp for the full formula.
//
// num_files: count of input files this run will process (<=0 = unknown -> treated as
// the conservative multi-file case). Pass todo.size()/inputs.size() at the call site.
//
// Multi-file (num_files >= P+1) preserves the hard no-oversubscription invariant
// in_flight + gpu_feed + cpu_asr_threads <= C. The few-files branch may briefly
// overshoot during the short decode phase (the extra producers exit before ASR
// dominates), but never sustains a 2x oversubscription.
//
// Call decide() first if you want the full auto-tune; call fill_hetero() when
// the caller already knows it wants the Hetero path (e.g. --provider hetero).
void fill_hetero(AutoTune& t, const HardwareInfo& hw, int num_files);

// Data-parallel CPU-consumer split for the Qwen3-ASR model ONLY. When cfg selects
// Qwen3 (cfg.qwen3_encoder non-empty) AND t.provider==Cpu, set t.cpu_consumers=K and
// split t.num_threads = clamp(total_logical/K, 2, total_logical) so K independent CPU
// recognizers share the cores. For all OTHER models (or non-CPU provider) this is a
// no-op: cpu_consumers stays 1 and num_threads is untouched. total_logical is the
// machine's logical core count (hw.cpu_threads). Idempotent / safe to call after a
// CPU recompute in the CLI/GUI. K is the benchmarked optimum (see hardware.cpp).
void set_qwen3_cpu_consumers(AutoTune& t, const EngineConfig& cfg, int total_logical);
}
