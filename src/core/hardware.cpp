#include "core/hardware.h"
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
  return h;
}
AutoTune decide(const HardwareInfo& hw, const EngineConfig& cfg){
  AutoTune t;
  bool use_gpu = hw.has_cuda_gpu && hw.gpu_free_mb >= 3000;
  if(use_gpu){
    t.provider = Provider::Cuda;
    t.num_threads = 1;
    // batch starts at 8; scale up by free VRAM, leave ~1.5GB headroom, ~150MB per activation chunk
    int headroom = hw.gpu_free_mb - 1500;
    int by_vram = headroom>0 ? headroom/150 : 0;
    t.batch = std::max(8, std::min(32, by_vram));
  } else {
    t.provider = Provider::Cpu;
    t.num_threads = std::max(4, hw.cpu_threads);   // CPU decode uses multi-thread
    t.batch = std::max(1, std::min(4, hw.cpu_threads/4)); // CPU batch benefit limited
  }
  // in-flight files: scale with available RAM (~2GB/file budget), clamped to [2,8]
  int by_ram = hw.ram_free_mb>0 ? hw.ram_free_mb/2000 : 2;
  t.in_flight_files = std::max(2, std::min(8, by_ram));
  (void)cfg; // reserved for future override hooks
  return t;
}
}
