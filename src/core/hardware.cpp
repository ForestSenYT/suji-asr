#include "core/hardware.h"
#include "core/paths.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <string>
#include <sstream>
#include <algorithm>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace suji {
static bool run_capture(const std::string& cmd, std::string& out){
  out.clear();
  FILE* p = _popen(cmd.c_str(), "r");
  if(!p) return false;
  char buf[512]; size_t n;
  while((n=fread(buf,1,sizeof(buf),p))>0) out.append(buf,n);
  _pclose(p);
  return !out.empty();
}
HardwareInfo probe_hardware(const std::string& nvidia_smi){
  HardwareInfo h;
  h.cpu_threads = (int)std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
  MEMORYSTATUSEX ms; ms.dwLength=sizeof(ms);
  if(GlobalMemoryStatusEx(&ms)){ h.ram_total_mb=(int)(ms.ullTotalPhys/(1024*1024)); h.ram_free_mb=(int)(ms.ullAvailPhys/(1024*1024)); }
#endif
  // nvidia-smi --query-gpu=name,memory.total,memory.free --format=csv,noheader,nounits
  std::string out;
  std::string cmd = "\"" + nvidia_smi + "\" --query-gpu=name,memory.total,memory.free --format=csv,noheader,nounits 2>nul";
  if(run_capture(cmd, out)){
    // first line: "NVIDIA GeForce RTX 2080, 8192, 6000"
    std::istringstream ls(out); std::string line;
    if(std::getline(ls,line) && line.find(',')!=std::string::npos){
      std::istringstream fs(line); std::string name,tot,fre;
      std::getline(fs,name,','); std::getline(fs,tot,','); std::getline(fs,fre,',');
      auto trim=[](std::string s){ size_t a=s.find_first_not_of(" \t\r\n"); size_t b=s.find_last_not_of(" \t\r\n"); return a==std::string::npos?std::string():s.substr(a,b-a+1); };
      try { h.gpu_total_mb=std::stoi(trim(tot)); h.gpu_free_mb=std::stoi(trim(fre)); h.gpu_name=trim(name); h.has_cuda_gpu = h.gpu_total_mb>0; } catch(...) {}
    }
  }
  h.cuda_dll_dir = cuda_dll_dir();
  h.cuda_runtime_available = !h.cuda_dll_dir.empty();
  return h;
}
AutoTune decide(const HardwareInfo& hw, const EngineConfig& cfg){
  AutoTune t;
  const int C = hw.cpu_threads;
  bool gpu_ok = hw.has_cuda_gpu && hw.gpu_free_mb >= 3000 && hw.cuda_runtime_available;
  bool cpu_ok = C >= 12;

  if(gpu_ok && cpu_ok){
    // Hetero: one CPU recognizer + one CUDA recognizer in parallel.
    // R1 fix: compute in_flight_files FIRST, then derive cpu_asr_threads,
    // so the later RAM clamp cannot retroactively break the invariant.
    t.hetero         = true;
    t.provider       = Provider::Hetero;
    int P            = std::clamp(C / 4, 2, 6);   // producers == in_flight_files
    t.in_flight_files = P;
    const int gpu_feed = 1;                        // threads consumed by CUDA consumer
    t.cpu_asr_threads = std::max(2, C - P - gpu_feed);
    t.cpu_batch       = std::clamp(t.cpu_asr_threads / 2, 2, 6);
    int headroom      = hw.gpu_free_mb - 1500;
    t.gpu_batch       = std::clamp(headroom > 0 ? headroom / 150 : 8, 8, 32);
    t.num_threads     = t.cpu_asr_threads;         // legacy mirror
    t.batch           = t.gpu_batch;               // legacy mirror
    // Hard postcondition: no oversubscription
    assert(t.in_flight_files + gpu_feed + t.cpu_asr_threads <= C);
  } else if(gpu_ok){
    t.provider    = Provider::Cuda;
    t.num_threads = 1;
    // batch starts at 8; scale up by free VRAM, leave ~1.5GB headroom, ~150MB per activation chunk
    int headroom  = hw.gpu_free_mb - 1500;
    int by_vram   = headroom > 0 ? headroom / 150 : 0;
    t.batch       = std::max(8, std::min(32, by_vram));
    // in-flight files: scale with available RAM (~2GB/file budget), clamped to [2,8]
    // R1 fix: only the non-hetero branches use the RAM clamp.
    int by_ram      = hw.ram_free_mb > 0 ? hw.ram_free_mb / 2000 : 2;
    t.in_flight_files = std::max(2, std::min(8, by_ram));
  } else {
    t.provider    = Provider::Cpu;
    t.num_threads = std::max(4, C);                // CPU decode uses multi-thread
    t.batch       = std::max(1, std::min(4, C / 4)); // CPU batch benefit limited
    // R1 fix: only the non-hetero branches use the RAM clamp.
    int by_ram      = hw.ram_free_mb > 0 ? hw.ram_free_mb / 2000 : 2;
    t.in_flight_files = std::max(2, std::min(8, by_ram));
  }
  (void)cfg; // reserved for future override hooks
  return t;
}
}
