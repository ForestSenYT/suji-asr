#include "core/batch_engine.h"
#include "core/batch_form.h"
#include "core/bounded_queue.h"
#include "core/media_decode.h"
#include "core/vad.h"
#include "core/asr.h"
#include "core/oom_retry.h"
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

// UTF-8-safe basename (no extension strip): last component after the last '/' or '\'.
// Operates on raw UTF-8 bytes (ASCII-transparent) so Chinese filenames survive.
// std::filesystem narrow path would re-interpret UTF-8 as the system ANSI codepage.
static std::string basename_utf8(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

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
  // G13: total DECODED audio duration in samples (full file length incl. silence),
  // accumulated by producers. Drives the FINAL aggregate throughput (true audio-hours).
  std::atomic<long long> decoded_samples{0};
  // Segment-based progress: producer +1 to segs_total before each push; consumer +1 to
  // segs_done per segment routed. segs_done==segs_total on a clean run -> bar hits 100%.
  std::atomic<long long> segs_total{0};
  std::atomic<long long> segs_done{0};
  // PER-FILE segment progress ("每个视频分开"): same lock-free pattern as the global
  // counters, just split by file_id. Producer +1 to seg_total_pf[fi] before each push;
  // consumer +1 to seg_done_pf[b.file_id] per routed segment. INVARIANT (clean run):
  // Σ seg_done_pf == segs_done and Σ seg_total_pf == segs_total. atomics aren't copyable,
  // so a unique_ptr array (mirrors seg_pending) keeps the hot path lock-free.
  std::unique_ptr<std::atomic<long long>[]> seg_total_pf(new std::atomic<long long>[N]);
  std::unique_ptr<std::atomic<long long>[]> seg_done_pf(new std::atomic<long long>[N]);
  for(int i=0;i<N;i++){ seg_total_pf[i].store(0); seg_done_pf[i].store(0); }
  // Snapshot every file's (index, done, total) into bp.files for the per-file GUI bars.
  auto fill_files = [&](BatchProgress& bp){
    bp.files.reserve((size_t)N);
    for(int i=0;i<N;i++)
      bp.files.push_back({i, seg_done_pf[i].load(), seg_total_pf[i].load()});
  };

  // producers: decode + VAD; push SegTask; record failures
  int nprod = std::max(1, tune.in_flight_files);
  std::vector<std::thread> producers;
  for(int t=0;t<nprod;t++){
    producers.emplace_back([&]{
      // T11: build the Silero VAD ONCE per producer thread, not per file. The model
      // is reloaded only nprod times total instead of once per input. Vad::segment()
      // calls Reset() internally, so reusing one detector across files is safe.
      Vad vad(cfg);
      size_t fi;
      while((fi=next_file.fetch_add(1)) < (size_t)N){
        if(cancel && cancel->is_cancelled()) break;   // stop taking new files on cancel
        if(!vad.ok()){ std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="VAD init"; continue; }
        std::string err;
        log_info("解码: " + basename_utf8(inputs[fi]));   // C1: decode begins
        // P3: STREAMING decode+VAD. ffmpeg's PCM is fed INCREMENTALLY into the VAD as it
        // decodes; each speech segment is pushed to the queue the instant it is found, so
        // consumers start transcribing within seconds even for a multi-hour file, and we
        // never hold the whole file in RAM. Per-segment bookkeeping is unchanged; segment
        // start_sample stays global (the stream is fed continuously, no offset math).
        // queue.push blocks on a full queue = backpressure; false from the callback on
        // cancel stops emission + terminates ffmpeg.
        long long produced = 0;
        int64_t file_samples = 0;
        bool ok = decode_vad_stream(cfg.ffmpeg_path, inputs[fi], vad, [&](SpeechSeg&& s) -> bool {
          if(cancel && cancel->is_cancelled()) return false;   // stop emitting on cancel
          seg_pending[fi].fetch_add(1);              // R6 fix: count before push
          segs_total.fetch_add(1);                   // segment-based progress: total grows as files are VADed
          seg_total_pf[fi].fetch_add(1);             // per-file: this file's total grows too
          SegTask st; st.file_id=(int)fi; st.start_sample=s.start_sample; st.samples=std::move(s.samples);
          queue.push(std::move(st));                 // blocks if queue full (backpressure)
          ++produced;
          return true;
        }, err, cancel, &file_samples);
        if(!ok){
          std::lock_guard<std::mutex> lk(err_mu);
          results[fi].ok=false;
          results[fi].err = (err=="cancelled") ? "cancelled" : "decode: "+err;
          continue;
        }
        // G13: count the full decoded duration of this file (incl. silence) for throughput.
        decoded_samples += (long long)file_samples;
        log_info("切分完成: " + basename_utf8(inputs[fi]) + " (" + std::to_string(produced) + " 段)");  // C1: VAD done
        // R6: this file's production is complete only if we pushed every segment.
        { std::lock_guard<std::mutex> lk(err_mu); produced_complete[fi]=1; }
      }
    });
  }
  // consumer: batch decode, route tokens by file_id
  // G2: the cuda-only single path gets OOM halve-retry; the CPU path keeps its exact
  // prior behaviour (a plain transcribe_batch call, no retry) so CPU is unaffected.
  const bool gpu_retry = (tune.provider == Provider::Cuda);
  auto consumer_last_cb = std::chrono::steady_clock::time_point{};  // zero = never fired
  std::thread consumer([&]{
    // P5: transcribe one already-formed batch -> route tokens -> bump counters -> live cb.
    // Identical work whether the batch came from a mid-run emit or the EOF flush, so it
    // lives in one place to keep R3/R6 bookkeeping in exactly one spot.
    auto process_batch = [&](std::vector<SegTask>&& batch){
      std::vector<Asr::SegView> views; views.reserve(batch.size());
      for(auto& b : batch) views.push_back({b.samples.data(),(int)b.samples.size()});
      auto res = gpu_retry
        ? transcribe_oom_safe(views, [&](const std::vector<Asr::SegView>& v){ return asr.transcribe_batch(v); })
        : asr.transcribe_batch(views);
      if (res.size() != batch.size()){   // R3/T12: batch failed -> mark files, never a silent empty success
        mark_batch_failed(batch, res.size(), results, err_mu);
        return;
      }
      route_batch_tokens(batch, res, file_tokens);
      for(auto& b : batch){
        samples_done += b.samples.size();
        seg_pending[b.file_id].fetch_sub(1);         // R6 fix: segment fully consumed
        segs_done.fetch_add(1);                      // segment-based progress: one segment routed
        seg_done_pf[b.file_id].fetch_add(1);         // per-file: this file's done grows too
      }
      // live progress: throttle to ~150 ms so we don't flood the GUI
      if(cb){
        auto now = std::chrono::steady_clock::now();
        if(consumer_last_cb == std::chrono::steady_clock::time_point{} ||
           std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_cb).count() >= 150){
          consumer_last_cb = now;
          BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load();
          bp.audio_seconds_done=(double)samples_done.load()/16000.0;
          bp.total_audio_decoded=(double)decoded_samples.load()/16000.0;
          bp.segs_done=segs_done.load(); bp.segs_total=segs_total.load();
          fill_files(bp);
          cb(bp);
        }
      }
    };
    // P5 gate (real). bucket = (N==1): length-bucketing only helps a single-file run; for
    // N>1 it regressed the multi-file work-stealing balance, so we restore the EXACT pre-P5
    // FIFO fast path there (arrival-order batches, no hold/sort). form_next_batch encodes the
    // gate; the bucket path holds (cap 2x batch) + sorts by sample length, the FIFO path takes
    // `first` + try_pop up to bmax-1 more in arrival order. Tokens route per-segment by file_id,
    // so reordering (when on) never changes attribution.
    const bool bucket = (N == 1);
    const int bmax = std::max(1, tune.batch);
    auto len_of = [](const SegTask& s){ return s.samples.size(); };
    std::vector<SegTask> hold;
    bool broke_on_cancel = false;
    SegTask first;
    while(queue.pop(first)){
      if(cancel && cancel->is_cancelled()){ queue.close(); broke_on_cancel = true; break; }  // close releases blocked producers
      // NOTE: neither `first` nor anything left in `hold` is decremented on the cancel path —
      // that's intentional. seg_pending stays elevated for every popped-but-unrouted segment
      // (the discarded `first` AND any held buffer), so finalize classifies those files cancelled.
      auto batch = form_next_batch(bucket, std::move(first), [&](SegTask& o){ return queue.try_pop(o); }, bmax, hold, len_of);
      if(!batch.empty()) process_batch(std::move(batch));
      // bucket mode may now hold >= bmax again only after another pop; flush full batches eagerly.
      while((int)hold.size() >= bmax){ auto b = flush_held(bmax, hold, len_of); if(b.empty()) break; process_batch(std::move(b)); }
    }
    if(!broke_on_cancel)                             // clean EOF (drained naturally): flush remainder, never drop a held segment
      for(auto b = flush_held(bmax, hold, len_of); !b.empty(); b = flush_held(bmax, hold, len_of))
        process_batch(std::move(b));
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
      log_info("完成: " + basename_utf8(inputs[i]) + " (" + std::to_string(results[i].transcript.segments.size()) + " 段)");  // C1
    }
    files_done++;                                  // count every file (ok or failed)
    if(cb){ BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load(); bp.audio_seconds_done=(double)samples_done.load()/16000.0; bp.total_audio_decoded=(double)decoded_samples.load()/16000.0; bp.segs_done=segs_done.load(); bp.segs_total=segs_total.load(); fill_files(bp); cb(bp); }
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
  // G13: total DECODED audio duration (full file length incl. silence), accumulated
  // by producers. Drives the FINAL aggregate throughput (true audio-hours).
  std::atomic<long long> decoded_samples{0};
  // H9: per-consumer segment counters for split-ratio observability
  std::atomic<long long> cpu_segs{0}, gpu_segs{0};
  // Segment-based progress: producer +1 to segs_total before each push; consumers +1 to
  // segs_done per segment routed. segs_done==segs_total on a clean run -> bar hits 100%.
  std::atomic<long long> segs_total{0};
  std::atomic<long long> segs_done{0};
  // PER-FILE segment progress ("每个视频分开"): split the global counters by file_id.
  // Producer +1 to seg_total_pf[fi] before push; EITHER consumer +1 to seg_done_pf[file_id]
  // when it routes that segment. Each element is touched lock-free (atomic). INVARIANT:
  // Σ seg_done_pf == segs_done and Σ seg_total_pf == segs_total. Two consumers writing to
  // distinct atomic elements (or the same element atomically) — no new race vs the globals.
  std::unique_ptr<std::atomic<long long>[]> seg_total_pf(new std::atomic<long long>[N]);
  std::unique_ptr<std::atomic<long long>[]> seg_done_pf(new std::atomic<long long>[N]);
  for(int i=0;i<N;i++){ seg_total_pf[i].store(0); seg_done_pf[i].store(0); }
  auto fill_files = [&](BatchProgress& bp){
    bp.files.reserve((size_t)N);
    for(int i=0;i<N;i++)
      bp.files.push_back({i, seg_done_pf[i].load(), seg_total_pf[i].load()});
  };

  // producers: identical to the single path.
  int nprod = std::max(1, tune.in_flight_files);
  std::vector<std::thread> producers;
  for(int t=0;t<nprod;t++){
    producers.emplace_back([&]{
      // T11: build the Silero VAD ONCE per producer thread (see single path). Reused
      // across files; Vad::segment() Resets internally so per-file segmentation is independent.
      Vad vad(cfg);
      size_t fi;
      while((fi=next_file.fetch_add(1)) < (size_t)N){
        if(cancel && cancel->is_cancelled()) break;
        if(!vad.ok()){ std::lock_guard<std::mutex> lk(err_mu); results[fi].ok=false; results[fi].err="VAD init"; continue; }
        std::string err;
        log_info("解码: " + basename_utf8(inputs[fi]));   // C1: decode begins
        // P3: STREAMING decode+VAD (see single path). ffmpeg's PCM is fed INCREMENTALLY
        // into the VAD; each segment is pushed to the shared queue the instant it is found,
        // so BOTH the CPU and GPU consumers start transcribing within seconds of a long
        // file starting, with no whole-file memory spike. push blocks on a full queue =
        // backpressure; false from the callback stops emission + terminates ffmpeg.
        long long produced = 0;
        int64_t file_samples = 0;
        bool ok = decode_vad_stream(cfg.ffmpeg_path, inputs[fi], vad, [&](SpeechSeg&& s) -> bool {
          if(cancel && cancel->is_cancelled()) return false;   // stop emitting on cancel
          seg_pending[fi].fetch_add(1);              // R6 fix: count before push
          segs_total.fetch_add(1);                   // segment-based progress: total grows as files are VADed
          seg_total_pf[fi].fetch_add(1);             // per-file: this file's total grows too
          SegTask st; st.file_id=(int)fi; st.start_sample=s.start_sample; st.samples=std::move(s.samples);
          queue.push(std::move(st));                 // blocks if queue full (backpressure)
          ++produced;
          return true;
        }, err, cancel, &file_samples);
        if(!ok){
          std::lock_guard<std::mutex> lk(err_mu);
          results[fi].ok=false;
          results[fi].err = (err=="cancelled") ? "cancelled" : "decode: "+err;
          continue;
        }
        // G13: count the full decoded duration of this file (incl. silence) for throughput.
        decoded_samples += (long long)file_samples;
        log_info("切分完成: " + basename_utf8(inputs[fi]) + " (" + std::to_string(produced) + " 段)");  // C1: VAD done
        { std::lock_guard<std::mutex> lk(err_mu); produced_complete[fi]=1; }
      }
    });
  }

  // One reusable consumer body, parameterized by (recognizer, batch_max, sink, seg_counter,
  // oom_retry). Each recognizer handle is touched by exactly ONE thread.
  // G2: oom_retry=true (GPU consumer only) wraps transcribe in the halve-retry helper;
  // the CPU consumer passes oom_retry=false and keeps its exact prior behaviour.
  std::atomic<bool> cb_lock{false};                  // tiny spinlock to serialize the throttled cb
  auto consumer_last_cb = std::chrono::steady_clock::time_point{};
  auto consume = [&](Asr& asr, int batch_max, std::vector<std::vector<Token>>& sink,
                     std::atomic<long long>& seg_counter, bool oom_retry){
    // P5: transcribe one already-formed batch -> route -> bump counters -> live cb. Same
    // work whether the batch came from a mid-run emit or the EOF flush, so it lives once.
    auto process_batch = [&](std::vector<SegTask>&& batch){
      std::vector<Asr::SegView> views; views.reserve(batch.size());
      for(auto& b : batch) views.push_back({b.samples.data(),(int)b.samples.size()});
      auto res = oom_retry
        ? transcribe_oom_safe(views, [&](const std::vector<Asr::SegView>& v){ return asr.transcribe_batch(v); })
        : asr.transcribe_batch(views);
      if(res.size() != batch.size()){   // R3/T12
        mark_batch_failed(batch, res.size(), results, err_mu);
        return;
      }
      route_batch_tokens(batch, res, sink);
      for(auto& b : batch){
        samples_done += b.samples.size();
        seg_pending[b.file_id].fetch_sub(1);         // R6 fix: segment fully consumed
        segs_done.fetch_add(1);                      // segment-based progress: one segment routed
        seg_done_pf[b.file_id].fetch_add(1);         // per-file: this file's done grows too
      }
      seg_counter += (long long)batch.size();        // H9: count segments processed by this consumer
      if(cb && !cb_lock.exchange(true)){              // best-effort throttled live cb
        auto now = std::chrono::steady_clock::now();
        if(consumer_last_cb == std::chrono::steady_clock::time_point{} ||
           std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_cb).count() >= 150){
          consumer_last_cb = now;
          BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load();
          bp.audio_seconds_done=(double)samples_done.load()/16000.0;
          bp.total_audio_decoded=(double)decoded_samples.load()/16000.0;
          bp.cpu_segs=cpu_segs.load(); bp.gpu_segs=gpu_segs.load();   // G14: live split
          bp.segs_done=segs_done.load(); bp.segs_total=segs_total.load();
          fill_files(bp);
          cb(bp);
        }
        cb_lock.store(false);
      }
    };
    // P5 gate (real). bucket = (N==1): for N>1 length-bucketing here regressed the CPU/GPU
    // work-stealing split (a holding consumer hoards stealable work -> aggregate throughput
    // drops), so the multi-file path is the EXACT pre-P5 FIFO fast path (arrival-order batches,
    // no hold/sort). For a single file the bucket path holds (cap 2x this consumer's batch) +
    // sorts by sample length -> less padding waste. form_next_batch encodes the gate.
    const bool bucket = (N == 1);
    const int bmax = std::max(1, batch_max);
    auto len_of = [](const SegTask& s){ return s.samples.size(); };
    std::vector<SegTask> hold;
    bool broke_on_cancel = false;
    SegTask first;
    while(queue.pop(first)){
      if(cancel && cancel->is_cancelled()){ queue.close(); broke_on_cancel = true; break; }
      // NOTE: neither `first` nor anything left in `hold` is decremented on the cancel path —
      // seg_pending stays elevated for every popped-but-unrouted segment (the discarded `first`
      // AND any held buffer), so finalize classifies those files cancelled. Nothing is lost.
      auto batch = form_next_batch(bucket, std::move(first), [&](SegTask& o){ return queue.try_pop(o); }, bmax, hold, len_of);
      if(!batch.empty()) process_batch(std::move(batch));
      while((int)hold.size() >= bmax){ auto b = flush_held(bmax, hold, len_of); if(b.empty()) break; process_batch(std::move(b)); }
    }
    if(!broke_on_cancel)                             // clean EOF (drained naturally): flush remainder, never drop a held segment
      for(auto b = flush_held(bmax, hold, len_of); !b.empty(); b = flush_held(bmax, hold, len_of))
        process_batch(std::move(b));
  };

  std::vector<std::thread> consumers;
  // G2: CPU consumer -> no OOM retry (unchanged); CUDA consumer -> halve-retry on OOM.
  if(cok) consumers.emplace_back([&]{ consume(cpu_asr, std::max(1,tune.cpu_batch), tok_cpu, cpu_segs, /*oom_retry=*/false); });
  if(gok) consumers.emplace_back([&]{ consume(gpu_asr, std::max(1,tune.gpu_batch), tok_gpu, gpu_segs, /*oom_retry=*/true); });

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
      log_info("完成: " + basename_utf8(inputs[i]) + " (" + std::to_string(results[i].transcript.segments.size()) + " 段)");  // C1
    }
    files_done++;
    if(cb){ BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load(); bp.audio_seconds_done=(double)samples_done.load()/16000.0; bp.total_audio_decoded=(double)decoded_samples.load()/16000.0; bp.cpu_segs=cpu_segs.load(); bp.gpu_segs=gpu_segs.load(); bp.segs_done=segs_done.load(); bp.segs_total=segs_total.load(); fill_files(bp); cb(bp); }
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
