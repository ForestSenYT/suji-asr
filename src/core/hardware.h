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
}
