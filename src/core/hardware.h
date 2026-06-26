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
// t.cpu_batch, t.gpu_batch, t.num_threads, t.batch using the same formula as
// decide()'s hetero branch (P=clamp(C/4,2,6), cpu_asr=max(2,C-P-1), etc.).
// Call decide() first if you want the full auto-tune; call fill_hetero() when
// the caller already knows it wants the Hetero path (e.g. --provider hetero).
void fill_hetero(AutoTune& t, const HardwareInfo& hw);
}
