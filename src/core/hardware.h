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
}
