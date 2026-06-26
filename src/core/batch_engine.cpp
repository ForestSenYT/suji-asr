#include "core/batch_engine.h"
#include "core/bounded_queue.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/asr.h"
#include "core/punctuation.h"
#include "core/segment_merge.h"
#include "core/log.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <string>
#include <iterator>
namespace suji {
namespace {
struct SegTask { int file_id; int64_t start_sample; std::vector<float> samples; };

// Route one transcribe_batch result into a per-file token sink (no global lock;
// each consumer owns its own sink). tk.start = seg start (s) + local timestamp.
void route_batch_tokens(const std::vector<SegTask>& batch,
                        const std::vector<AsrResult>& res,
                        std::vector<std::vector<Token>>& sink){
  for(size_t i=0;i<batch.size();++i){
    double base = (double)batch[i].start_sample/16000.0;
    for(size_t k=0;k<res[i].tokens.size() && k<res[i].timestamps.size();++k){
      Token tk; tk.text=res[i].tokens[k]; tk.start=base+res[i].timestamps[k];
      sink[batch[i].file_id].push_back(tk);
    }
  }
}

// R3 + T12: a wrong-size batch result means the batch FAILED (e.g. stream
// creation hit VRAM pressure — asr.cpp now returns an empty vector in that case).
// Mark every distinct file in the batch failed; NEVER record it as a successful
// empty result. Guarded by err_mu because results[] is shared across consumers.
void mark_batch_failed(const std::vector<SegTask>& batch, size_t got,
                       std::vector<FileResult>& results, std::mutex& err_mu){
  log_err("transcribe_batch returned wrong size (expected " +
          std::to_string(batch.size()) + ", got " + std::to_string(got) +
          "); marking " + std::to_string(batch.size()) + "-segment batch failed");
  std::lock_guard<std::mutex> lk(err_mu);
  int last = -1;   // batches are built file-by-file from the queue; dedupe runs of same id cheaply
  for(const auto& b : batch){
    if(b.file_id == last) continue;
    last = b.file_id;
    results[b.file_id].ok = false;
    results[b.file_id].err = "transcribe failed";
  }
}

std::vector<FileResult> transcribe_batch_files_single(const std::vector<std::string>& inputs,
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb, CancelToken* cancel){
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
  std::vector<char> produced_complete(N,0);          // R6: set when ALL of a file's segs pushed
  // R6 fix: per-file pending-segment counter. Incremented by producer BEFORE push,
  // decremented by consumer AFTER routing tokens. On cancel the discarded `first`
  // is NOT decremented, so seg_pending[fi] > 0 catches the truncation window that
  // produced_complete alone misses (produced_complete only tracks push-completion).
  std::unique_ptr<std::atomic<int>[]> seg_pending(new std::atomic<int>[N]);
  for(int i=0;i<N;i++) seg_pending[i].store(0);
  std::mutex err_mu;                                 // guards results[].ok/err + produced_complete
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
        if(cancel && cancel->is_cancelled()) break;   // stop taking new files on cancel
        AudioBuffer ab; std::string err;
        if(!decode_to_pcm(cfg.ffmpeg_path, inputs[fi], ab, err, cancel)){
          std::lock_guard<std::mutex> lk(err_mu);
          results[fi].ok=false;
          results[fi].err = (err=="cancelled") ? "cancelled" : "decode: "+err;
          continue;
        }
        // A just-decoded file must not enter VAD if cancel landed during decode.
        if(cancel && cancel->is_cancelled()){
          std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="cancelled"; continue;
        }
        Vad vad(cfg);
        if(!vad.ok()){ std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="VAD init"; continue; }
        auto segs = vad.segment(ab, cancel);
        for(auto& s : segs){
          seg_pending[fi].fetch_add(1);              // R6 fix: count before push
          SegTask st; st.file_id=(int)fi; st.start_sample=s.start_sample; st.samples=std::move(s.samples);
          queue.push(std::move(st));
        }
        // R6: this file's production is complete only if we pushed every segment.
        { std::lock_guard<std::mutex> lk(err_mu); produced_complete[fi]=1; }
      }
    });
  }
  // consumer: batch decode, route tokens by file_id
  auto consumer_last_cb = std::chrono::steady_clock::time_point{};  // zero = never fired
  std::thread consumer([&]{
    SegTask first;
    while(queue.pop(first)){
      if(cancel && cancel->is_cancelled()){ queue.close(); break; }  // close releases blocked producers
      // NOTE: `first` is NOT decremented on the cancel path above — that's intentional.
      // seg_pending[first.file_id] stays elevated, catching this popped-but-discarded segment.
      std::vector<SegTask> batch; batch.push_back(std::move(first));
      SegTask more;
      while((int)batch.size() < tune.batch && queue.try_pop(more)) batch.push_back(std::move(more));
      std::vector<Asr::SegView> views; views.reserve(batch.size());
      for(auto& b : batch) views.push_back({b.samples.data(),(int)b.samples.size()});
      auto res = asr.transcribe_batch(views);
      if (res.size() != batch.size()){   // R3/T12: batch failed -> mark files, never a silent empty success
        mark_batch_failed(batch, res.size(), results, err_mu);
        continue;
      }
      route_batch_tokens(batch, res, file_tokens);
      for(auto& b : batch){
        samples_done += b.samples.size();
        seg_pending[b.file_id].fetch_sub(1);         // R6 fix: segment fully consumed
      }
      // live progress: throttle to ~150 ms so we don't flood the GUI
      if(cb){
        auto now = std::chrono::steady_clock::now();
        if(consumer_last_cb == std::chrono::steady_clock::time_point{} ||
           std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_cb).count() >= 150){
          consumer_last_cb = now;
          BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load();
          bp.audio_seconds_done=(double)samples_done.load()/16000.0;
          cb(bp);
        }
      }
    }
  });
  for(auto& p:producers) p.join();
  queue.close();
  consumer.join();

  // finalize per file (single-threaded): sort tokens by time -> merge -> punctuate
  Punctuator punct(cfg);
  for(int i=0;i<N;i++){
    // R6 fix: use seg_pending (consumption tracking) in addition to produced_complete.
    // seg_pending[i] > 0 catches the window where all segs were pushed (produced_complete=1)
    // but tail segs were still queued or were the popped-but-discarded `first` on cancel.
    // produced_complete[i]==0 catches the window where pushing itself was interrupted.
    // Together they cover every truncation scenario; clean runs leave seg_pending==0.
    if (cancel && cancel->is_cancelled() && results[i].ok &&
        (seg_pending[i].load() > 0 || !produced_complete[i])) {
      results[i].ok = false; results[i].err = "cancelled";
    }
    if(results[i].ok){
      auto& toks = file_tokens[i];
      std::sort(toks.begin(), toks.end(), [](const Token&a,const Token&b){ return a.start<b.start; });
      Transcript tr;
      tr.segments = merge_tokens(toks, cfg.merge_gap, cfg.merge_max_dur);
      for(auto& seg : tr.segments){ seg.text = punct.add(seg.text); tr.full_text += seg.text; }
      results[i].transcript = std::move(tr);
    }
    files_done++;                                  // count every file (ok or failed)
    if(cb){ BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load(); bp.audio_seconds_done=(double)samples_done.load()/16000.0; cb(bp); }
  }
  return results;
}

// ---------------------------------------------------------------------------
// H3 — heterogeneous path: one CPU recognizer + one CUDA recognizer pulling
// from a single shared queue (pull-based work-stealing; the faster engine
// naturally drains more). Each recognizer handle is touched by exactly ONE
// thread; a single segment is pop'd exactly once -> no double-processing.
// ---------------------------------------------------------------------------
std::vector<FileResult> transcribe_batch_files_hetero(const std::vector<std::string>& inputs,
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb, CancelToken* cancel){
  const int N=(int)inputs.size();
  std::vector<FileResult> results(N);
  for(int i=0;i<N;i++){ results[i].input=inputs[i]; results[i].ok=true; }
  if(N==0) return results;

  // Build TWO recognizers, CPU first then CUDA (deterministic per H0).
  EngineConfig cpu_cfg = cfg; cpu_cfg.provider=Provider::Cpu; cpu_cfg.num_threads=std::max(1,tune.cpu_asr_threads);
  EngineConfig gpu_cfg = cfg; gpu_cfg.provider=Provider::Cuda; gpu_cfg.num_threads=1; gpu_cfg.cuda_dll_dir=cfg.cuda_dll_dir;
  Asr cpu_asr(cpu_cfg);
  Asr gpu_asr(gpu_cfg);
  bool cok=cpu_asr.ok(), gok=gpu_asr.ok();
  // Graceful degradation: both dead -> fail all; only one ok -> run that one consumer.
  if(!cok && !gok){
    log_err("hetero: both CPU and CUDA recognizers failed to initialise");
    for(auto& r:results){ r.ok=false; r.err="ASR init failed"; }
    return results;
  }
  if(!gok) log_err("hetero: CUDA recognizer unavailable; running CPU-only consumer");
  if(!cok) log_err("hetero: CPU recognizer unavailable; running CUDA-only consumer");

  // R4: queue cap large enough to stay saturated under both batch sizes and the
  // producer fan-out, so the GPU's opportunistic try_pop can't starve the CPU.
  size_t cap = std::max({(size_t)4,
                         (size_t)(tune.cpu_batch+tune.gpu_batch)*4,
                         (size_t)tune.in_flight_files*8});
  BoundedQueue<SegTask> queue(cap);

  // Two token sinks: each consumer writes ONLY its own (hot path lock-free).
  std::vector<std::vector<Token>> tok_cpu(N), tok_gpu(N);
  std::vector<char> produced_complete(N,0);          // R6
  // R6 fix: per-file pending-segment counter (same semantics as single path).
  // Producer increments before push; consumer decrements after routing. On cancel,
  // the discarded `first` is never decremented -> seg_pending > 0 catches truncation.
  std::unique_ptr<std::atomic<int>[]> seg_pending(new std::atomic<int>[N]);
  for(int i=0;i<N;i++) seg_pending[i].store(0);
  std::mutex err_mu;                                 // guards results[] + produced_complete
  std::atomic<size_t> next_file{0};
  std::atomic<int> files_done{0};
  std::atomic<long long> samples_done{0};
  // H9: per-consumer segment counters for split-ratio observability
  std::atomic<long long> cpu_segs{0}, gpu_segs{0};

  // producers: identical to the single path.
  int nprod = std::max(1, tune.in_flight_files);
  std::vector<std::thread> producers;
  for(int t=0;t<nprod;t++){
    producers.emplace_back([&]{
      size_t fi;
      while((fi=next_file.fetch_add(1)) < (size_t)N){
        if(cancel && cancel->is_cancelled()) break;
        AudioBuffer ab; std::string err;
        if(!decode_to_pcm(cfg.ffmpeg_path, inputs[fi], ab, err, cancel)){
          std::lock_guard<std::mutex> lk(err_mu);
          results[fi].ok=false;
          results[fi].err = (err=="cancelled") ? "cancelled" : "decode: "+err;
          continue;
        }
        if(cancel && cancel->is_cancelled()){
          std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="cancelled"; continue;
        }
        Vad vad(cfg);
        if(!vad.ok()){ std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="VAD init"; continue; }
        auto segs = vad.segment(ab, cancel);
        for(auto& s : segs){
          seg_pending[fi].fetch_add(1);              // R6 fix: count before push
          SegTask st; st.file_id=(int)fi; st.start_sample=s.start_sample; st.samples=std::move(s.samples);
          queue.push(std::move(st));
        }
        { std::lock_guard<std::mutex> lk(err_mu); produced_complete[fi]=1; }
      }
    });
  }

  // One reusable consumer body, parameterized by (recognizer, batch_max, sink, seg_counter).
  // Each recognizer handle is touched by exactly ONE thread.
  std::atomic<bool> cb_lock{false};                  // tiny spinlock to serialize the throttled cb
  auto consumer_last_cb = std::chrono::steady_clock::time_point{};
  auto consume = [&](Asr& asr, int batch_max, std::vector<std::vector<Token>>& sink,
                     std::atomic<long long>& seg_counter){
    SegTask first;
    while(queue.pop(first)){
      if(cancel && cancel->is_cancelled()){ queue.close(); break; }
      // NOTE: `first` is NOT decremented on the cancel path — seg_pending[first.file_id]
      // stays elevated to catch this popped-but-discarded segment at finalize.
      std::vector<SegTask> batch; batch.push_back(std::move(first));
      SegTask more;
      while((int)batch.size() < batch_max && queue.try_pop(more)) batch.push_back(std::move(more));
      std::vector<Asr::SegView> views; views.reserve(batch.size());
      for(auto& b : batch) views.push_back({b.samples.data(),(int)b.samples.size()});
      auto res = asr.transcribe_batch(views);
      if(res.size() != batch.size()){   // R3/T12
        mark_batch_failed(batch, res.size(), results, err_mu);
        continue;
      }
      route_batch_tokens(batch, res, sink);
      for(auto& b : batch){
        samples_done += b.samples.size();
        seg_pending[b.file_id].fetch_sub(1);         // R6 fix: segment fully consumed
      }
      seg_counter += (long long)batch.size();        // H9: count segments processed by this consumer
      if(cb && !cb_lock.exchange(true)){              // best-effort throttled live cb
        auto now = std::chrono::steady_clock::now();
        if(consumer_last_cb == std::chrono::steady_clock::time_point{} ||
           std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_cb).count() >= 150){
          consumer_last_cb = now;
          BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load();
          bp.audio_seconds_done=(double)samples_done.load()/16000.0;
          cb(bp);
        }
        cb_lock.store(false);
      }
    }
  };

  std::vector<std::thread> consumers;
  if(cok) consumers.emplace_back([&]{ consume(cpu_asr, std::max(1,tune.cpu_batch), tok_cpu, cpu_segs); });
  if(gok) consumers.emplace_back([&]{ consume(gpu_asr, std::max(1,tune.gpu_batch), tok_gpu, gpu_segs); });

  // Join order: producers -> close -> both consumers.
  for(auto& p:producers) p.join();
  queue.close();
  for(auto& c:consumers) c.join();

  // H9: log the CPU/GPU segment split for observability (visible in CLI stderr + GUI log).
  {
    long long cs = cpu_segs.load(), gs = gpu_segs.load(), tot = cs + gs;
    if (tot > 0) {
      int cpu_pct = (int)(cs * 100 / tot), gpu_pct = (int)(gs * 100 / tot);
      log_info("hetero split: CPU " + std::to_string(cs) + " segs (" + std::to_string(cpu_pct)
               + "%) / GPU " + std::to_string(gs) + " segs (" + std::to_string(gpu_pct) + "%)");
    } else {
      log_info("hetero split: CPU 0 segs (0%) / GPU 0 segs (0%)");
    }
  }

  // finalize per file: merge tok_cpu[i] + tok_gpu[i] -> sort -> merge -> punctuate.
  Punctuator punct(cfg);
  for(int i=0;i<N;i++){
    // R6 fix: seg_pending[i] > 0 catches the truncation window where all segs were
    // pushed (produced_complete=1) but tail segs are still queued or were the
    // popped-but-discarded `first`. produced_complete[i]==0 covers the push-interrupted
    // case. A clean (non-cancelled) run leaves seg_pending[i]==0 for every file.
    if (cancel && cancel->is_cancelled() && results[i].ok &&
        (seg_pending[i].load() > 0 || !produced_complete[i])) {
      results[i].ok = false; results[i].err = "cancelled";   // R6: truncated transcript is NOT ok
    }
    if(results[i].ok){
      std::vector<Token> toks = std::move(tok_cpu[i]);
      toks.insert(toks.end(), std::make_move_iterator(tok_gpu[i].begin()),
                              std::make_move_iterator(tok_gpu[i].end()));
      std::sort(toks.begin(), toks.end(), [](const Token&a,const Token&b){ return a.start<b.start; });
      Transcript tr;
      tr.segments = merge_tokens(toks, cfg.merge_gap, cfg.merge_max_dur);
      for(auto& seg : tr.segments){ seg.text = punct.add(seg.text); tr.full_text += seg.text; }
      results[i].transcript = std::move(tr);
    }
    files_done++;
    if(cb){ BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load(); bp.audio_seconds_done=(double)samples_done.load()/16000.0; cb(bp); }
  }
  return results;
}
}  // namespace

std::vector<FileResult> transcribe_batch_files(const std::vector<std::string>& inputs,
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb, CancelToken* cancel){
  if(tune.provider == Provider::Hetero)
    return transcribe_batch_files_hetero(inputs, cfg, tune, cb, cancel);
  return transcribe_batch_files_single(inputs, cfg, tune, cb, cancel);
}
}  // namespace suji
