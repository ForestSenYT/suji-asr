#include "core/hardware.h"
#include "core/paths.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <string>
#include <sstream>
#include <algorithm>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace suji {
#ifdef _WIN32
// Convert a UTF-8 std::string to a UTF-16 std::wstring.
static std::wstring utf8_to_wide(const std::string& utf8){
  if(utf8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if(n <= 0) return {};
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
  return w;
}
// Run `nvidia_smi <args>` via CreateProcessW (NO cmd.exe shell, no ANSI codepage),
// capturing its stdout into `out`. Mirrors the media_decode.cpp CreateProcessW pattern:
// inheritable stdout pipe, NUL for stderr, CREATE_NO_WINDOW, ReadFile loop.
// Returns false (and leaves out empty/partial) on any failure -> no-GPU safe fallback.
static bool run_capture(const std::string& nvidia_smi, const std::wstring& args, std::string& out){
  out.clear();
  std::wstring exe_w = utf8_to_wide(nvidia_smi);
  if(exe_w.empty()) return false;

  // Build a mutable command line: "<nvidia-smi>" <args>. lpApplicationName is NULL so the
  // PATH is searched (nvidia-smi is normally on PATH). Quote the exe to tolerate spaces.
  std::wstring cmdline = L"\"" + exe_w + L"\" " + args;

  SECURITY_ATTRIBUTES sa{};
  sa.nLength        = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE pipe_read  = INVALID_HANDLE_VALUE;
  HANDLE pipe_write = INVALID_HANDLE_VALUE;
  if(!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) return false;
  if(!SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0)){
    CloseHandle(pipe_read); CloseHandle(pipe_write); return false;
  }

  // stderr -> NUL so nvidia-smi diagnostics never pollute the captured stdout.
  HANDLE nul_handle = CreateFileW(L"NUL", GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa, OPEN_EXISTING, 0, nullptr);
  if(nul_handle == INVALID_HANDLE_VALUE){
    CloseHandle(pipe_read); CloseHandle(pipe_write); return false;
  }

  STARTUPINFOW si{};
  si.cb         = sizeof(si);
  si.dwFlags    = STARTF_USESTDHANDLES;
  si.hStdInput  = nullptr;
  si.hStdOutput = pipe_write;
  si.hStdError  = nul_handle;

  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                           TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

  CloseHandle(pipe_write);
  CloseHandle(nul_handle);

  if(!ok){ CloseHandle(pipe_read); return false; }   // nvidia-smi missing -> no-GPU fallback

  // Read nvidia-smi's stdout (ASCII CSV) until EOF.
  std::vector<char> chunk(512);
  DWORD bytes_read = 0;
  while(ReadFile(pipe_read, chunk.data(), static_cast<DWORD>(chunk.size()), &bytes_read, nullptr)
        && bytes_read > 0){
    out.append(chunk.data(), bytes_read);
  }
  CloseHandle(pipe_read);

  WaitForSingleObject(pi.hProcess, INFINITE);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return !out.empty();
}
#else
static bool run_capture(const std::string&, const std::wstring&, std::string& out){
  out.clear();
  return false;   // non-Windows: no nvidia-smi probe -> no-GPU fallback
}
#endif
HardwareInfo probe_hardware(const std::string& nvidia_smi){
  HardwareInfo h;
  h.cpu_threads = (int)std::max(1u, std::thread::hardware_concurrency());
#ifdef _WIN32
  MEMORYSTATUSEX ms; ms.dwLength=sizeof(ms);
  if(GlobalMemoryStatusEx(&ms)){ h.ram_total_mb=(int)(ms.ullTotalPhys/(1024*1024)); h.ram_free_mb=(int)(ms.ullAvailPhys/(1024*1024)); }
#endif
  // nvidia-smi --query-gpu=name,memory.total,memory.free --format=csv,noheader,nounits
  std::string out;
  if(run_capture(nvidia_smi,
                 L"--query-gpu=name,memory.total,memory.free --format=csv,noheader,nounits",
                 out)){
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
void fill_hetero(AutoTune& t, const HardwareInfo& hw) {
  const int C = hw.cpu_threads;
  t.hetero          = true;
  t.provider        = Provider::Hetero;
  int P             = std::clamp(C / 4, 2, 6);   // producers == in_flight_files
  t.in_flight_files = P;
  const int gpu_feed = 1;                         // threads consumed by CUDA consumer
  t.cpu_asr_threads = std::max(2, C - P - gpu_feed);
  t.cpu_batch       = std::clamp(t.cpu_asr_threads / 2, 2, 6);
  int headroom      = hw.gpu_free_mb - 1500;
  t.gpu_batch       = std::clamp(headroom > 0 ? headroom / 150 : 8, 8, 32);
  t.num_threads     = t.cpu_asr_threads;          // legacy mirror
  t.batch           = t.gpu_batch;                // legacy mirror
  // Hard postcondition: no oversubscription
  assert(t.in_flight_files + gpu_feed + t.cpu_asr_threads <= C);
}

AutoTune decide(const HardwareInfo& hw, const EngineConfig& cfg){
  AutoTune t;
  const int C = hw.cpu_threads;
  bool gpu_ok = hw.has_cuda_gpu && hw.gpu_free_mb >= 3000 && hw.cuda_runtime_available;
  bool cpu_ok = C >= 12;

  if(gpu_ok && cpu_ok){
    // Hetero: one CPU recognizer + one CUDA recognizer in parallel.
    // Delegate to fill_hetero so the CLI can reuse the same formula.
    fill_hetero(t, hw);
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
  // T5: apply optional caps (0 = uncapped/auto)
  if (cfg.max_batch   > 0) {
    t.batch           = std::min(t.batch,           cfg.max_batch);
    t.gpu_batch       = std::min(t.gpu_batch,       cfg.max_batch);
    t.cpu_batch       = std::min(t.cpu_batch,       cfg.max_batch);
  }
  if (cfg.max_threads > 0) {
    t.num_threads     = std::min(t.num_threads,     cfg.max_threads);
    t.cpu_asr_threads = std::min(t.cpu_asr_threads, cfg.max_threads);
  }
  return t;
}
}
