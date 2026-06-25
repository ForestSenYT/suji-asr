# Phase 2 — GPU 批量 + 硬件自适应 批处理引擎 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]`.

**Goal:** 把单文件流水线升级为**多文件批处理引擎**:CPU 多线程喂数(decode+VAD)→ 共享队列 → 单 GPU 消费者**批量解码**(`SherpaOnnxDecodeMultipleOfflineStreams`)→ 按文件路由 → 每文件 Transcript。并**按机器自适应**选 provider(CUDA↔CPU)/batch/在飞数/线程,GPU 不可用时优雅回退 CPU。

**Architecture:** 生产者-消费者。N 个生产者线程各取一个文件:`decode_to_pcm`+`Vad::segment` → 把每个 VAD 段(PCM+file_id+start_sample)推入**有界阻塞队列**(背压控内存)。1 个消费者线程独占 recognizer:`pop` 一个段后再非阻塞 `try_pop` 凑满至多 B 个 → `Asr::transcribe_batch`(一次 DecodeMultiple)→ 按 file_id 把字级 token(全局时间)累加到该文件。全部生产者结束→关队列→消费者排空。收尾(主线程,单线程):每文件 token 按时间排序 → `merge_tokens` → 逐段标点 → Transcript。

**Tech Stack:** C++17 · std::thread/mutex/condition_variable · sherpa-onnx v1.13.3 C API(`DecodeMultipleOfflineStreams`)· 复用 Phase-1 的 `decode_to_pcm`/`Vad`/`Asr`/`Punctuator`/`merge_tokens`/writers。

## Global Constraints
- 平台 Windows x64,MSVC(VS2022),C++17,`/utf-8`,`/W4` 输出 pristine。cmake 全路径 `F:\Git\suji-asr\vendor\cmake-4.3.3-windows-x86_64\bin\cmake.exe`(不在 PATH)。
- **复用 Phase-1 单元,勿重写**:`core/media_decode.h`(decode_to_pcm)、`core/vad.h`(Vad)、`core/asr.h`(Asr)、`core/punctuation.h`(Punctuator)、`core/segment_merge.h`(merge_tokens)、`core/output/writer_facade.h`(write_outputs)、`core/config.h`、`core/types.h`。
- **批量 API**(verified c-api.h):`void SherpaOnnxDecodeMultipleOfflineStreams(const SherpaOnnxOfflineRecognizer*, const SherpaOnnxOfflineStream**, int32_t n)`。每段建一个 stream + `AcceptWaveformOffline`,收集指针数组,调用一次,再逐 stream `GetOfflineStreamResult`/`DestroyResult`/`DestroyStream`。
- **测试一律 CPU 模式**(`provider=cpu`):批量 API 在 CPU/CUDA 同源;确定性 + 免 CUDA DLL。GPU 路径同代码,Phase 4 单独验证。
- **单消费者独占 recognizer**(避免 ORT CUDA 并发问题)。生产者**不**碰 recognizer。
- **全局时间戳**:`token.start = segtask.start_sample/16000.0 + local_ts[k]`。跨文件批量交错到达 → 收尾时**每文件 token 先按 .start 排序**再 merge_tokens(单文件 Phase-1 无需排序,这里必须)。
- DLL:构建后 `lib/` 已自动拷到 exe 同目录(含 onnxruntime + sherpa-onnx-c-api + cargs)。CPU 测试无需额外 DLL。
- DRY / YAGNI / TDD / 频繁本地提交(不 push)。Phase 2 **不做**:断点续跑/ETA 美化(Phase 3)、benchmark 报告(Phase 4)、GUI(Phase 5)。

## File Structure
```
src/core/hardware.h .cpp        # HardwareInfo + probe_hardware(); AutoTune + decide()
src/core/bounded_queue.h        # BoundedQueue<T>(header-only template)
src/core/asr.h .cpp             # (MODIFY) + Asr::transcribe_batch(...)
src/core/batch_engine.h .cpp    # FileResult, transcribe_batch_files(...)
src/cli/batch_main.cpp          # suji_batch CLI(目录/多文件 → 自适应 → 批量 → 每文件输出 + 聚合吞吐)
tests/test_autotune.cpp         # pure-logic TDD for decide()
tests/test_bounded_queue.cpp    # threaded TDD for BoundedQueue
tests/integration/test_hardware.cpp     # probe on this box
tests/integration/test_batch_asr.cpp    # batch decode == single-stream, CPU
tests/integration/test_batch_engine.cpp # multi-file batch on test_wavs, CPU
```
CMake: `suji_core` globs `src/core/*.cpp` (recursive) — new files auto-picked. Add a new exe `suji_batch` from `src/cli/batch_main.cpp` linking `suji_core` (+ `suji_copy_runtime_dlls`). Tests glob `tests/*.cpp` recursive.

---

## Task 1: Hardware probe + auto-tune policy

**Files:** Create `src/core/hardware.h`, `src/core/hardware.cpp`, `tests/test_autotune.cpp`, `tests/integration/test_hardware.cpp`

**Interfaces — Produces:**
```cpp
// hardware.h
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
};
struct AutoTune { Provider provider = Provider::Cpu; int batch = 1; int in_flight_files = 1; int num_threads = 1; };
HardwareInfo probe_hardware(const std::string& nvidia_smi = "nvidia-smi");
AutoTune decide(const HardwareInfo& hw, const EngineConfig& cfg);
}
```

- [ ] **Step 1: autotune failing test** `tests/test_autotune.cpp`
```cpp
#include "doctest/doctest.h"
#include "core/hardware.h"
using namespace suji;
static HardwareInfo hw(bool gpu,int freemb,int cores,int ramfree){ HardwareInfo h; h.has_cuda_gpu=gpu; h.gpu_free_mb=freemb; h.gpu_total_mb=8192; h.cpu_threads=cores; h.ram_free_mb=ramfree; h.ram_total_mb=ramfree; return h; }
TEST_CASE("autotune picks GPU when enough VRAM") {
  auto t = decide(hw(true, 6000, 16, 40000), EngineConfig{});
  CHECK(t.provider == Provider::Cuda);
  CHECK(t.num_threads == 1);          // GPU -> 1
  CHECK(t.batch >= 8);
}
TEST_CASE("autotune falls to CPU when no GPU") {
  auto t = decide(hw(false, 0, 16, 40000), EngineConfig{});
  CHECK(t.provider == Provider::Cpu);
  CHECK(t.num_threads >= 4);          // uses cores
  CHECK(t.batch >= 1);
  CHECK(t.in_flight_files >= 1);
}
TEST_CASE("autotune falls to CPU when VRAM too low") {
  auto t = decide(hw(true, 1000, 16, 40000), EngineConfig{}); // <3GB free
  CHECK(t.provider == Provider::Cpu);
}
TEST_CASE("autotune in-flight scales with RAM but stays bounded") {
  auto t = decide(hw(true, 6000, 16, 40000), EngineConfig{});
  CHECK(t.in_flight_files >= 2);
  CHECK(t.in_flight_files <= 8);
}
```

- [ ] **Step 2: build → fail** (link error). Run `...\cmake.exe --build build --config Release`.

- [ ] **Step 3: implement** `src/core/hardware.h` (above) + `src/core/hardware.cpp`:
```cpp
#include "core/hardware.h"
#include <cstdio>
#include <thread>
#include <string>
#include <sstream>
#include <algorithm>
#ifdef _WIN32
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
    // batch 起 8;按空闲 VRAM 上调,留 ~1.5GB 余量,每段激活粗估 ~150MB
    int headroom = hw.gpu_free_mb - 1500;
    int by_vram = headroom>0 ? headroom/150 : 0;
    t.batch = std::max(8, std::min(32, by_vram));
  } else {
    t.provider = Provider::Cpu;
    t.num_threads = std::max(4, hw.cpu_threads);   // CPU 解码用多线程
    t.batch = std::max(1, std::min(4, hw.cpu_threads/4)); // CPU 批量收益有限
  }
  // 在飞文件数:按可用 RAM(每多小时文件 ~350MB),夹在 [2,8]
  int by_ram = hw.ram_free_mb>0 ? hw.ram_free_mb/2000 : 2;  // 保守 2GB/文件预算
  t.in_flight_files = std::max(2, std::min(8, by_ram));
  return t;
}
}
```

- [ ] **Step 4: hardware probe integration test** `tests/integration/test_hardware.cpp`
```cpp
#include "doctest/doctest.h"
#include "core/hardware.h"
using namespace suji;
TEST_CASE("probe returns sane values on this box" * doctest::timeout(30)){
  auto h = probe_hardware();
  CHECK(h.cpu_threads >= 1);
  CHECK(h.ram_total_mb > 1000);
  // this dev box has an RTX 2080; if nvidia-smi present, gpu fields populated. Don't hard-require GPU.
  if(h.has_cuda_gpu){ CHECK(h.gpu_total_mb > 1000); CHECK(h.gpu_name.size() > 0); }
}
```

- [ ] **Step 5: build → run** `build\Release\suji_tests.exe -tc="autotune*","probe*"` → all pass. Full suite green.
- [ ] **Step 6: commit** `git add src/core/hardware.h src/core/hardware.cpp tests/test_autotune.cpp tests/integration/test_hardware.cpp && git commit -m "feat: hardware probe + auto-tune policy (CUDA/CPU)"`

---

## Task 2: Bounded blocking queue (header-only)

**Files:** Create `src/core/bounded_queue.h`, `tests/test_bounded_queue.cpp`

**Interfaces — Produces:**
```cpp
// bounded_queue.h
#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
namespace suji {
template<class T> class BoundedQueue {
public:
  explicit BoundedQueue(size_t cap): cap_(cap?cap:1) {}
  void push(T v){                                   // blocks while full and not closed
    std::unique_lock<std::mutex> lk(m_);
    notfull_.wait(lk, [&]{ return q_.size()<cap_ || closed_; });
    if(closed_) return;
    q_.push(std::move(v)); notempty_.notify_one();
  }
  bool pop(T& out){                                 // blocks for 1; false iff closed && empty
    std::unique_lock<std::mutex> lk(m_);
    notempty_.wait(lk, [&]{ return !q_.empty() || closed_; });
    if(q_.empty()) return false;
    out=std::move(q_.front()); q_.pop(); notfull_.notify_one(); return true;
  }
  bool try_pop(T& out){                              // non-blocking; false if empty
    std::lock_guard<std::mutex> lk(m_);
    if(q_.empty()) return false;
    out=std::move(q_.front()); q_.pop(); notfull_.notify_one(); return true;
  }
  void close(){ { std::lock_guard<std::mutex> lk(m_); closed_=true; } notempty_.notify_all(); notfull_.notify_all(); }
private:
  size_t cap_; std::queue<T> q_; std::mutex m_;
  std::condition_variable notempty_, notfull_; bool closed_=false;
};
}
```

- [ ] **Step 1: threaded failing test** `tests/test_bounded_queue.cpp`
```cpp
#include "doctest/doctest.h"
#include "core/bounded_queue.h"
#include <thread>
#include <atomic>
#include <vector>
using namespace suji;
TEST_CASE("producers/consumer transfer all items, close drains") {
  BoundedQueue<int> q(8);
  const int N=1000, P=4;
  std::atomic<int> produced{0};
  std::vector<std::thread> prod;
  for(int t=0;t<P;t++) prod.emplace_back([&]{ int x; while((x=produced.fetch_add(1))<N) q.push(x); });
  std::atomic<long long> sum{0}; std::atomic<int> got{0};
  std::thread cons([&]{ int v; while(q.pop(v)){ sum+=v; got++; } });
  for(auto& t:prod) t.join();
  q.close();
  cons.join();
  CHECK(got == N);
  CHECK(sum == (long long)N*(N-1)/2);   // 0+1+...+(N-1)
}
TEST_CASE("pop returns false when closed and empty") {
  BoundedQueue<int> q(4); q.close(); int v; CHECK_FALSE(q.pop(v));
}
TEST_CASE("try_pop batches what's available") {
  BoundedQueue<int> q(8); q.push(1); q.push(2);
  int a,b,c; CHECK(q.try_pop(a)); CHECK(q.try_pop(b)); CHECK_FALSE(q.try_pop(c));
}
```

- [ ] **Step 2: build → fail.** **Step 3: implement** header (above). **Step 4: build → run** `-tc="*producers*","*pop returns*","*try_pop*"` → pass; full suite green.
- [ ] **Step 5: commit** `git add src/core/bounded_queue.h tests/test_bounded_queue.cpp && git commit -m "feat: bounded blocking queue (producer-consumer)"`

---

## Task 3: `Asr::transcribe_batch` (batch decode)

**Files:** Modify `src/core/asr.h`, `src/core/asr.cpp`; Create `tests/integration/test_batch_asr.cpp`

**Interfaces — Produces (add to class Asr):**
```cpp
struct SegView { const float* samples; int n; };
std::vector<AsrResult> transcribe_batch(const std::vector<SegView>& segs);  // one result per seg, same order
```

- [ ] **Step 1: failing test** `tests/integration/test_batch_asr.cpp` (CPU; batch == single-stream)
```cpp
#include "doctest/doctest.h"
#include "core/asr.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/config.h"
using namespace suji;
static EngineConfig cfg(){ EngineConfig c; std::string md=SUJI_DEFAULT_MODELS_DIR; std::string m=md+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG; c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt"; c.vad_model=md+"/silero_vad.onnx"; c.provider=Provider::Cpu; c.num_threads=4; return c; }
TEST_CASE("batch decode matches single-stream" * doctest::timeout(180)){
  auto c=cfg(); AudioBuffer ab; std::string err;
  REQUIRE(decode_to_pcm(c.ffmpeg_path, std::string(SUJI_DEFAULT_MODELS_DIR)+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/0.wav", ab, err));
  Vad vad(c); auto segs=vad.segment(ab); REQUIRE(segs.size()>=2);
  Asr asr(c); REQUIRE(asr.ok());
  std::vector<Asr::SegView> views; for(auto& s:segs) views.push_back({s.samples.data(),(int)s.samples.size()});
  auto batch = asr.transcribe_batch(views);
  REQUIRE(batch.size()==segs.size());
  // compare against single-stream for each
  for(size_t i=0;i<segs.size();++i){
    auto single = asr.transcribe(segs[i].samples.data(),(int)segs[i].samples.size());
    CHECK(batch[i].text == single.text);
    CHECK(batch[i].tokens.size() == batch[i].timestamps.size());
  }
}
```

- [ ] **Step 2: build → fail.**
- [ ] **Step 3: implement.** In `asr.h` add inside `class Asr` (public): the `SegView` struct + `std::vector<AsrResult> transcribe_batch(const std::vector<SegView>& segs);`. In `asr.cpp`:
```cpp
std::vector<AsrResult> Asr::transcribe_batch(const std::vector<SegView>& segs){
  std::vector<AsrResult> out(segs.size());
  if(!rec_ || segs.empty()) return out;
  std::vector<const SherpaOnnxOfflineStream*> streams;
  streams.reserve(segs.size());
  for(auto& s : segs){
    const SherpaOnnxOfflineStream* st = SherpaOnnxCreateOfflineStream(rec_);
    SherpaOnnxAcceptWaveformOffline(st, 16000, s.samples, s.n);
    streams.push_back(st);
  }
  SherpaOnnxDecodeMultipleOfflineStreams(rec_, streams.data(), (int)streams.size());
  for(size_t i=0;i<streams.size();++i){
    const SherpaOnnxOfflineRecognizerResult* r = SherpaOnnxGetOfflineStreamResult(streams[i]);
    if(r){
      if(r->text) out[i].text=r->text;
      int count=r->count;
      for(int k=0;k<count;++k){
        out[i].tokens.push_back(r->tokens_arr && r->tokens_arr[k] ? r->tokens_arr[k] : "");
        out[i].timestamps.push_back(r->timestamps ? (double)r->timestamps[k] : 0.0);
      }
      SherpaOnnxDestroyOfflineRecognizerResult(r);
    }
    SherpaOnnxDestroyOfflineStream(streams[i]);
  }
  return out;
}
```
- [ ] **Step 4: build → run** `-tc="batch decode*"` → pass; full suite green.
- [ ] **Step 5: commit** `git add src/core/asr.h src/core/asr.cpp tests/integration/test_batch_asr.cpp && git commit -m "feat: Asr::transcribe_batch via DecodeMultipleOfflineStreams"`

---

## Task 4: Batch engine (multi-file producer-consumer)

**Files:** Create `src/core/batch_engine.h`, `src/core/batch_engine.cpp`, `tests/integration/test_batch_engine.cpp`

**Interfaces — Produces:**
```cpp
// batch_engine.h
#pragma once
#include "core/types.h"
#include "core/config.h"
#include "core/hardware.h"
#include <string>
#include <vector>
#include <functional>
namespace suji {
struct FileResult { std::string input; bool ok=false; std::string err; Transcript transcript; };
struct BatchProgress { int files_total=0; int files_done=0; double audio_seconds_done=0; };
using ProgressCb = std::function<void(const BatchProgress&)>;
// Decodes+VADs files on producer threads, batches ASR on one consumer (owns recognizer),
// then per-file: sort tokens by time -> merge_tokens -> punctuate -> Transcript.
std::vector<FileResult> transcribe_batch_files(const std::vector<std::string>& inputs,
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb = nullptr);
}
```

**Implementation `batch_engine.cpp` (transcribe verbatim — threading is correctness-critical):**
```cpp
#include "core/batch_engine.h"
#include "core/bounded_queue.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/asr.h"
#include "core/punctuation.h"
#include "core/segment_merge.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
namespace suji {
namespace {
struct SegTask { int file_id; int64_t start_sample; std::vector<float> samples; };
}
std::vector<FileResult> transcribe_batch_files(const std::vector<std::string>& inputs,
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb){
  const int N=(int)inputs.size();
  std::vector<FileResult> results(N);
  for(int i=0;i<N;i++){ results[i].input=inputs[i]; results[i].ok=true; }
  if(N==0) return results;

  // recognizer config reflects the tuned provider/threads
  EngineConfig ecfg = cfg; ecfg.provider = tune.provider;
  ecfg.num_threads = (tune.provider==Provider::Cuda) ? 1 : tune.num_threads;
  Asr asr(ecfg);
  if(!asr.ok()){ for(auto& r:results){ r.ok=false; r.err="ASR init failed"; } return results; }

  BoundedQueue<SegTask> queue((size_t)std::max(4, tune.batch*4));
  std::vector<std::vector<Token>> file_tokens(N);   // consumer-only (no lock)
  std::mutex err_mu;                                 // guards results[].ok/err for producer writes
  std::atomic<size_t> next_file{0};
  std::atomic<int> files_done{0};
  std::atomic<long long> samples_done{0};

  // producers: decode + VAD; push SegTask; record failures
  int nprod = std::max(1, tune.in_flight_files);
  std::vector<std::thread> producers;
  for(int t=0;t<nprod;t++){
    producers.emplace_back([&]{
      size_t fi;
      while((fi=next_file.fetch_add(1)) < (size_t)N){
        AudioBuffer ab; std::string err;
        if(!decode_to_pcm(cfg.ffmpeg_path, inputs[fi], ab, err)){
          { std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="decode: "+err; }
          continue;
        }
        Vad vad(cfg);
        if(!vad.ok()){ std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="VAD init"; continue; }
        auto segs = vad.segment(ab);
        for(auto& s : segs){ SegTask st; st.file_id=(int)fi; st.start_sample=s.start_sample; st.samples=std::move(s.samples); queue.push(std::move(st)); }
      }
    });
  }
  // consumer: batch decode, route tokens by file_id
  std::thread consumer([&]{
    SegTask first;
    while(queue.pop(first)){
      std::vector<SegTask> batch; batch.push_back(std::move(first));
      SegTask more;
      while((int)batch.size() < tune.batch && queue.try_pop(more)) batch.push_back(std::move(more));
      std::vector<Asr::SegView> views; views.reserve(batch.size());
      for(auto& b : batch) views.push_back({b.samples.data(),(int)b.samples.size()});
      auto res = asr.transcribe_batch(views);
      for(size_t i=0;i<batch.size();++i){
        double base = (double)batch[i].start_sample/16000.0;
        for(size_t k=0;k<res[i].tokens.size() && k<res[i].timestamps.size();++k){
          Token tk; tk.text=res[i].tokens[k]; tk.start=base+res[i].timestamps[k];
          file_tokens[batch[i].file_id].push_back(tk);
        }
        samples_done += batch[i].samples.size();
      }
    }
  });
  for(auto& p:producers) p.join();
  queue.close();
  consumer.join();

  // finalize per file (single-threaded): sort tokens by time -> merge -> punctuate
  Punctuator punct(cfg);
  for(int i=0;i<N;i++){
    if(!results[i].ok) continue;
    auto& toks = file_tokens[i];
    std::sort(toks.begin(), toks.end(), [](const Token&a,const Token&b){ return a.start<b.start; });
    Transcript tr;
    tr.segments = merge_tokens(toks, cfg.merge_gap, cfg.merge_max_dur);
    for(auto& seg : tr.segments){ seg.text = punct.add(seg.text); tr.full_text += seg.text; }
    if(tr.segments.empty() && toks.empty()){ /* no speech detected — still ok=true, empty */ }
    results[i].transcript = std::move(tr);
    files_done++;
    if(cb){ BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load(); bp.audio_seconds_done=(double)samples_done.load()/16000.0; cb(bp); }
  }
  return results;
}
}
```

- [ ] **Step 1: failing test** `tests/integration/test_batch_engine.cpp`
```cpp
#include "doctest/doctest.h"
#include "core/batch_engine.h"
#include "core/config.h"
#include <string>
using namespace suji;
static std::string md(){ return SUJI_DEFAULT_MODELS_DIR; }
static EngineConfig cfg(){ EngineConfig c; std::string m=md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG; c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt"; c.vad_model=md()+"/silero_vad.onnx";
  c.punct_model=md()+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx"; c.provider=Provider::Cpu; c.num_threads=4; return c; }
TEST_CASE("batch transcribe multiple files (CPU)" * doctest::timeout(300)){
  std::string w=md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  std::vector<std::string> inputs={ w+"0.wav", w+"1.wav", w+"2.wav" };
  AutoTune tune; tune.provider=Provider::Cpu; tune.batch=4; tune.in_flight_files=2; tune.num_threads=4;
  auto res = transcribe_batch_files(inputs, cfg(), tune);
  REQUIRE(res.size()==3);
  for(auto& r : res){ CHECK(r.ok); CHECK_FALSE(r.transcript.full_text.empty()); }
}
TEST_CASE("batch isolates a bad file" * doctest::timeout(300)){
  std::string w=md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  std::vector<std::string> inputs={ w+"0.wav", "no_such_file.wav", w+"1.wav" };
  AutoTune tune; tune.provider=Provider::Cpu; tune.batch=4; tune.in_flight_files=2; tune.num_threads=4;
  auto res = transcribe_batch_files(inputs, cfg(), tune);
  REQUIRE(res.size()==3);
  CHECK(res[0].ok); CHECK_FALSE(res[1].ok); CHECK(res[2].ok);   // bad file isolated
  CHECK(res[1].err.size()>0);
}
```

- [ ] **Step 2: build → fail. Step 3: implement** (header + cpp above). **Step 4: build → run** `-tc="batch transcribe*","batch isolates*"` → pass (CPU; ~1-3 min, loads model). Full suite green.
- [ ] **Step 5: commit** `git add src/core/batch_engine.h src/core/batch_engine.cpp tests/integration/test_batch_engine.cpp && git commit -m "feat: multi-file batch engine (producer-consumer + batched decode + error isolation)"`

---

## Task 5: `suji_batch` CLI (auto-tune + GPU enable + fallback)

**Files:** Create `src/cli/batch_main.cpp`; Modify `CMakeLists.txt` (add `suji_batch` exe); Modify `src/core/config.h` (add optional `std::string cuda_dll_dir;`); Modify `src/core/asr.cpp` (AddDllDirectory + CPU fallback on cuda failure).

**Behavior:** `suji_batch <dir-or-files...> [-o out_dir] [--provider auto|cpu|cuda] [--batch N] [--in-flight N]`. Default `--provider auto` → `probe_hardware`+`decide`. Collects input files (a dir → its media files; or explicit file args). Runs `transcribe_batch_files`. Writes `<out_dir>/<stem>.{srt,vtt,json,md}` per ok file (reuse `write_outputs`). Prints per-file status + **aggregate throughput = total_audio_seconds / wall_seconds**. On cuda recognizer init failure, auto-fall back to CPU (log it).

- [ ] **Step 1: config + asr GPU-enable.** Add `std::string cuda_dll_dir;` to `EngineConfig`. In `asr.cpp` constructor, BEFORE creating a cuda recognizer: if `provider==Cuda` and `!cfg.cuda_dll_dir.empty()`, call (Windows) `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS|LOAD_LIBRARY_SEARCH_USER_DIRS); AddDllDirectory(widen(cfg.cuda_dll_dir));` (guard with `#ifdef _WIN32`, `#include <windows.h>`). The CPU fallback lives in the CLI (Step 3), not here (keep Asr a thin wrapper). No new test for this (covered by the GPU run in Phase 4); keep changes minimal + warning-clean.
- [ ] **Step 2: CLI** `src/cli/batch_main.cpp`:
```cpp
#include "core/hardware.h"
#include "core/batch_engine.h"
#include "core/output/writer_facade.h"
#include "core/config.h"
#include "core/log.h"
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <filesystem>
using namespace suji;
namespace fs = std::filesystem;
static std::string stem(const std::string& p){ fs::path q(p); return q.stem().string(); }
static bool is_media(const fs::path& p){
  static const char* ext[]={".mp4",".mkv",".mov",".flv",".avi",".webm",".ts",".m4a",".mp3",".wav",".flac",".aac",".ogg",".opus"};
  std::string e=p.extension().string(); for(auto& c:e) c=(char)tolower(c);
  for(auto x:ext) if(e==x) return true; return false;
}
int main(int argc,char**argv){
  if(argc<2){ std::puts("usage: suji_batch <dir|files...> [-o out_dir] [--provider auto|cpu|cuda] [--batch N] [--in-flight N]"); return 2; }
  EngineConfig c; std::string mdl=SUJI_DEFAULT_MODELS_DIR, m=mdl+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path=SUJI_DEFAULT_FFMPEG; c.asr_model=m+"model.int8.onnx"; c.tokens=m+"tokens.txt"; c.vad_model=mdl+"/silero_vad.onnx";
  c.punct_model=mdl+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  std::string out_dir="."; std::string prov="auto"; int fbatch=0, finflight=0; std::vector<std::string> inputs;
  for(int i=1;i<argc;++i){ std::string a=argv[i];
    if(a=="-o"&&i+1<argc) out_dir=argv[++i];
    else if(a=="--provider"&&i+1<argc) prov=argv[++i];
    else if(a=="--batch"&&i+1<argc) fbatch=atoi(argv[++i]);
    else if(a=="--in-flight"&&i+1<argc) finflight=atoi(argv[++i]);
    else if(a.size()>2 && a[0]=='-'&&a[1]=='-'){ log_err("unknown flag "+a); return 2; }
    else { // dir or file
      fs::path p(a);
      if(fs::is_directory(p)){ for(auto& e: fs::directory_iterator(p)) if(e.is_regular_file()&&is_media(e.path())) inputs.push_back(e.path().string()); }
      else inputs.push_back(a);
    }
  }
  if(inputs.empty()){ log_err("no input media files"); return 1; }
  // hardware auto-tune
  HardwareInfo hw = probe_hardware();
  AutoTune tune = decide(hw, c);
  if(prov=="cpu") tune.provider=Provider::Cpu; else if(prov=="cuda") tune.provider=Provider::Cuda;
  if(fbatch>0) tune.batch=fbatch; if(finflight>0) tune.in_flight_files=finflight;
  log_info("hw: gpu="+std::string(hw.has_cuda_gpu?hw.gpu_name:"none")+" cores="+std::to_string(hw.cpu_threads)+" ramMB="+std::to_string(hw.ram_free_mb));
  log_info("tune: provider="+std::string(provider_str(tune.provider))+" batch="+std::to_string(tune.batch)+" in_flight="+std::to_string(tune.in_flight_files)+" files="+std::to_string(inputs.size()));
  // (GPU CUDA dll dir: if Cuda, set c.cuda_dll_dir to the consolidated redist dir if present)
  // run
  auto t0=std::chrono::steady_clock::now();
  auto res = transcribe_batch_files(inputs, c, tune, [](const BatchProgress& b){
    std::fprintf(stderr,"\r[%d/%d] %.0fs audio done", b.files_done, b.files_total, b.audio_seconds_done);
  });
  auto t1=std::chrono::steady_clock::now();
  double wall=std::chrono::duration<double>(t1-t0).count();
  // write outputs + tally
  fs::create_directories(out_dir);
  int okc=0; for(auto& r:res){
    if(r.ok){ okc++; write_outputs(r.transcript, out_dir+"/"+stem(r.input), c, stem(r.input)); }
    else log_err("FAILED "+r.input+": "+r.err);
  }
  std::printf("\ndone: %d/%zu ok, wall=%.1fs\n", okc, res.size(), wall);
  return okc>0?0:1;
}
```
> 注:`--provider cuda` 时 CPU 回退由 `Asr::ok()==false` 触发——CLI 在 run 前可加:若 tune.provider==Cuda,先用一个临时 `EngineConfig{provider=Cuda}` 试创建一个 `Asr`,`ok()` 为假则 `tune.provider=Provider::Cpu` 并 log 回退。(实现时加这段 ~6 行。)聚合吞吐 = 总音频秒/wall;音频总时长可在写出时累加各 transcript 末段 end,或 benchmark(Phase 4)精算——本 Task 先打印 wall + ok 数即可。

- [ ] **Step 3: CMakeLists** add:
```cmake
add_executable(suji_batch src/cli/batch_main.cpp)
target_link_libraries(suji_batch PRIVATE suji_core)
suji_copy_runtime_dlls(suji_batch)
```
- [ ] **Step 4: build → real run (CPU)** on test_wavs dir:
```
build\Release\suji_batch.exe "models/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs" -o build/batch_out --provider cpu
```
Expect: prints hw/tune lines, per-file progress, `done: N/N ok`, and `build/batch_out/*.srt` etc. exist. Full suite still green.
- [ ] **Step 5: commit** `git add src/cli/batch_main.cpp CMakeLists.txt src/core/config.h src/core/asr.cpp && git commit -m "feat: suji_batch CLI (auto-tune, dir input, per-file outputs)"`

---

## Phase 2 完成验收
- 全部新测试 + Phase-1 测试 PASS(CPU 模式)。
- `suji_batch` 在 test_wavs 目录真跑出多文件输出 + 聚合 wall。
- 错误隔离:坏文件不拖垮整批。
- **GPU 实跑留 Phase 4**(benchmark):届时把 CUDA redist 整合进一个 dir,`--provider cuda` 验证 + 测吞吐。

## Self-Review
- 覆盖 spec Phase 2:生产者-消费者✓、DecodeMultipleOfflineStreams 批量✓、单消费者复用 recognizer✓、队尾自然 flush(try_pop 凑批)✓、硬件自适应 provider/batch/在飞数✓、OOM 减半[Phase 4 GPU 时加]、在飞数控内存(有界队列背压 + in_flight 生产者数)✓。
- 类型一致:HardwareInfo/AutoTune/SegView/SegTask/FileResult/BatchProgress;复用 Token/Segment/Transcript/EngineConfig。
- 跨文件 token 交错 → 收尾按 .start 排序后 merge(关键正确性点)✓。
- 风险:GPU 路径未在本 Phase 集成测试(CPU 同源 + Phase 4 验证);OOM-halve 留到 GPU 实测时加。
