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
#include <memory>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <string>
#include <iterator>
#include <cstdio>
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

// Tiny formatters for the live 转写中 log line (STEP 6). fmt1 = one-decimal speed (e.g.
// "6.2"); mmss = a clamped mm:ss ETA string. Kept file-local — only the log line uses them.
static std::string fmt1(double v){ char b[32]; std::snprintf(b,sizeof b,"%.1f",v); return b; }
static std::string mmss(double secs){
  if(secs < 0) secs = 0;
  long long s = (long long)secs;
  long long mm = s / 60, ss = s % 60;
  char b[32]; std::snprintf(b,sizeof b,"%02lld:%02lld",mm,ss); return b;
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
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb, CancelToken* cancel,
    const std::vector<double>* file_full_seconds){
  const int N=(int)inputs.size();
  std::vector<FileResult> results(N);
  for(int i=0;i<N;i++){ results[i].input=inputs[i]; results[i].ok=true; }
  if(N==0) return results;

  // STEP 6: wall-clock origin for the live 转写中 line's elapsed/speed (consumer-side).
  auto t0 = std::chrono::steady_clock::now();

  // recognizer config reflects the tuned provider/threads
  EngineConfig ecfg = cfg; ecfg.provider = tune.provider;
  ecfg.num_threads = (tune.provider==Provider::Cuda) ? 1 : tune.num_threads;

  // Data-parallel CPU consumers (Qwen3): build K independent Asr handles, each draining
  // the ONE shared queue into its OWN token sink. K=1 is the original single-consumer
  // path (one handle, one sink) byte-for-byte. CUDA never sets cpu_consumers>1, so the
  // GPU OOM-retry path below stays single-consumer. Graceful degradation: if some handles
  // fail to init we run with fewer; if ALL fail we mark every file failed (as before).
  const int K = std::max(1, tune.cpu_consumers);
  std::vector<std::unique_ptr<Asr>> asrs;
  asrs.reserve((size_t)K);
  for(int k=0;k<K;k++){
    auto a = std::make_unique<Asr>(ecfg);
    if(a->ok()) asrs.push_back(std::move(a));
    else log_err("consumer " + std::to_string(k) + ": ASR init failed; degrading consumer count");
  }
  if(asrs.empty()){ for(auto& r:results){ r.ok=false; r.err="ASR init failed"; } return results; }
  const int kc = (int)asrs.size();   // actual consumer count after degradation

  BoundedQueue<SegTask> queue((size_t)std::max(4, tune.batch*4));
  // One token sink PER consumer (indexed [k][file_id]); each consumer writes ONLY its own
  // (hot path lock-free). Concatenated per file at finalize, then stable-sorted by time in
  // merge_file_tokens — same K-sink merge pattern the hetero path uses for its 2 sinks.
  std::vector<std::vector<std::vector<Token>>> sinks((size_t)kc, std::vector<std::vector<Token>>(N));
  std::vector<char> produced_complete(N,0);          // R6: set when ALL of a file's segs pushed
  std::vector<char> file_logged(N,0);                // STEP 7: 转写:<file> logged once when seg_done_pf 0->1 (guarded by err_mu)
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
  // Per-file consumed-speech-samples counter (mirrors seg_done_pf EXACTLY: same lock-free
  // atomic array, bumped in the same process_batch loop). Drives the per-file TIME-based
  // bar (samples_done_pf/16000 / FilePstat.full_seconds). No new lock, no new race.
  std::unique_ptr<std::atomic<long long>[]> samples_done_pf(new std::atomic<long long>[N]);
  for(int i=0;i<N;i++) samples_done_pf[i].store(0);
  // Snapshot every file's (index, done, total) into bp.files for the per-file GUI bars.
  auto fill_files = [&](BatchProgress& bp){
    bp.files.reserve((size_t)N);
    for(int i=0;i<N;i++)
      bp.files.push_back({i, seg_done_pf[i].load(), seg_total_pf[i].load(),
                          file_full_seconds ? (*file_full_seconds)[i] : 0.0,
                          samples_done_pf[i].load()});
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
  // consumers: batch decode, route tokens by file_id into each consumer's OWN sink.
  // G2: the cuda-only single path gets OOM halve-retry; the CPU path keeps its exact
  // prior behaviour (a plain transcribe_batch call, no retry) so CPU is unaffected.
  // CUDA never runs >1 consumer (cpu_consumers is CPU-only), so gpu_retry implies kc==1.
  const bool gpu_retry = (tune.provider == Provider::Cuda);
  // Shared progress throttle across all K consumers: ONE mutex-guarded last-cb timestamp
  // so the consumers don't each fire the cb (any consumer may fire when ~150 ms is due).
  // With K==1 this is exactly the old single-timestamp throttle (the mutex is uncontended).
  std::mutex cb_mu;
  auto consumer_last_cb = std::chrono::steady_clock::time_point{};  // zero = never fired
  // STEP 6: SEPARATE ~1s timestamp for the live 转写中 log line. Lives inside the SAME
  // cb_mu critical section as consumer_last_cb (two cadences, one lock) so it is serialized
  // identically — never floods the 5000-block GUI log (1 line/s ceiling, not ~6-7/s).
  auto consumer_last_log = std::chrono::steady_clock::time_point{};
  // One reusable consumer body, parameterized by (recognizer, sink). Each Asr handle and
  // each sink is touched by EXACTLY ONE thread; a segment is pop'd from the shared queue
  // exactly once -> no double-processing across the K consumers (same contract as hetero).
  auto consume = [&](Asr& asr, std::vector<std::vector<Token>>& sink){
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
      route_batch_tokens(batch, res, sink);
      for(auto& b : batch){
        samples_done += b.samples.size();
        seg_pending[b.file_id].fetch_sub(1);         // R6 fix: segment fully consumed
        segs_done.fetch_add(1);                      // segment-based progress: one segment routed
        seg_done_pf[b.file_id].fetch_add(1);         // per-file: this file's done grows too
        samples_done_pf[b.file_id].fetch_add((long long)b.samples.size());  // per-file: consumed speech samples (time bar)
        // STEP 7: log 转写:<file> ONCE when this file's first segment is routed. Take err_mu
        // ONLY to test+set the flag, then RELEASE it before log_info (never fprintf+sink under
        // the per-file mutex). Gives a 解码 -> 切分完成 -> 转写 -> 完成 arc per file.
        bool first=false;
        { std::lock_guard<std::mutex> lk(err_mu); if(!file_logged[b.file_id]){ file_logged[b.file_id]=1; first=true; } }
        if(first) log_info(u8"转写: " + basename_utf8(inputs[b.file_id]));
      }
      // live progress: throttle to ~150 ms so we don't flood the GUI. Guarded by cb_mu so K
      // consumers share ONE throttle window (the snapshot reads atomics; the lock only
      // serializes the timestamp check + the cb call). try_lock keeps a busy consumer from
      // blocking on the cb — if another consumer holds it, this one just skips this update.
      if(cb){
        std::unique_lock<std::mutex> lk(cb_mu, std::try_to_lock);
        if(lk.owns_lock()){
          auto now = std::chrono::steady_clock::now();
          // One snapshot of the shared atomics drives BOTH the 150ms bar cb AND the 1s log,
          // so the log's numbers are the exact values that drove the bar (coherent by build).
          BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load();
          bp.audio_seconds_done=(double)samples_done.load()/16000.0;
          bp.total_audio_decoded=(double)decoded_samples.load()/16000.0;
          bp.segs_done=segs_done.load(); bp.segs_total=segs_total.load();
          if(consumer_last_cb == std::chrono::steady_clock::time_point{} ||
             std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_cb).count() >= 150){
            consumer_last_cb = now;
            fill_files(bp);
            cb(bp);
          }
          // STEP 6: live density line on a SEPARATE ~1s cadence (consumer_last_log), still
          // inside this same cb_mu lock so the timestamp is serialized exactly like
          // consumer_last_cb. MUST NOT move outside the lock (would race across consumers).
          if(consumer_last_log == std::chrono::steady_clock::time_point{} ||
             std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_log).count() >= 1000){
            consumer_last_log = now;
            double el = std::chrono::duration<double>(now - t0).count();
            double x  = el > 0 ? bp.audio_seconds_done / el : 0.0;
            double eta = bp.segs_done > 0
              ? el * (double)(bp.segs_total - bp.segs_done) / (double)bp.segs_done : 0.0;
            log_info(u8"转写中 " + std::to_string(bp.segs_done) + "/" + std::to_string(bp.segs_total)
                     + u8" 段 · " + fmt1(x) + "x · " + u8"剩余~" + mmss(eta));
          }
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
  };
  // Spawn kc consumer threads, each bound to its own Asr handle + own sink. kc==1 is the
  // original single-consumer path (one thread, one sink) with no behaviour change.
  std::vector<std::thread> consumers;
  consumers.reserve((size_t)kc);
  for(int k=0;k<kc;k++)
    consumers.emplace_back([&,k]{ consume(*asrs[k], sinks[k]); });
  for(auto& p:producers) p.join();
  queue.close();
  for(auto& c:consumers) c.join();

  // finalize per file (single-threaded): sort tokens by time -> merge -> punctuate
  Punctuator punct(cfg);
  // Qwen3-ASR is an LLM decoder that EMITS ITS OWN punctuation; running the CT-transformer
  // punct pass on top of it doubles every mark (。。 / ，，). Detect Qwen3 via its encoder
  // field and skip the redundant pass (also one less model pass). AED/CTC keep punctuating.
  const bool self_punct = !cfg.qwen3_encoder.empty();
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
      // Concat this file's tokens across ALL kc consumer sinks, then merge_file_tokens
      // stable-sorts by time -> ordering across consumers is correct (same as hetero's
      // tok_cpu+tok_gpu concat). kc==1 is just a single move of sinks[0][i] (no extra copy).
      std::vector<Token> toks = std::move(sinks[0][i]);
      for(int k=1;k<kc;k++)
        toks.insert(toks.end(), std::make_move_iterator(sinks[k][i].begin()),
                                std::make_move_iterator(sinks[k][i].end()));
      Transcript tr;
      tr.segments = merge_file_tokens(std::move(toks), cfg.merge_gap, cfg.merge_max_dur);
      for(auto& seg : tr.segments){ seg.text = self_punct ? seg.text : punct.add(seg.text); tr.full_text += seg.text; }
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
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb, CancelToken* cancel,
    const std::vector<double>* file_full_seconds){
  const int N=(int)inputs.size();
  std::vector<FileResult> results(N);
  for(int i=0;i<N;i++){ results[i].input=inputs[i]; results[i].ok=true; }
  if(N==0) return results;

  // STEP 6: wall-clock origin for the live 转写中 line's elapsed/speed (consumer-side).
  auto t0 = std::chrono::steady_clock::now();

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
  std::vector<char> file_logged(N,0);                // STEP 7: 转写:<file> logged once when seg_done_pf 0->1 (guarded by err_mu)
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
  // Per-file consumed-speech-samples counter (mirrors seg_done_pf EXACTLY). Either consumer
  // bumps element [file_id] atomically — no new race vs the per-file seg counters.
  std::unique_ptr<std::atomic<long long>[]> samples_done_pf(new std::atomic<long long>[N]);
  for(int i=0;i<N;i++) samples_done_pf[i].store(0);
  auto fill_files = [&](BatchProgress& bp){
    bp.files.reserve((size_t)N);
    for(int i=0;i<N;i++)
      bp.files.push_back({i, seg_done_pf[i].load(), seg_total_pf[i].load(),
                          file_full_seconds ? (*file_full_seconds)[i] : 0.0,
                          samples_done_pf[i].load()});
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
  // STEP 6: SEPARATE ~1s timestamp for the live 转写中 line, serialized inside the SAME
  // cb_lock spinlock as consumer_last_cb (two cadences, one critical section, no extra lock).
  auto consumer_last_log = std::chrono::steady_clock::time_point{};
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
        samples_done_pf[b.file_id].fetch_add((long long)b.samples.size());  // per-file: consumed speech samples (time bar)
        // STEP 7: log 转写:<file> ONCE when this file's first segment is routed by EITHER
        // consumer. err_mu test+set, RELEASED before log_info (never log under the mutex).
        bool first=false;
        { std::lock_guard<std::mutex> lk(err_mu); if(!file_logged[b.file_id]){ file_logged[b.file_id]=1; first=true; } }
        if(first) log_info(u8"转写: " + basename_utf8(inputs[b.file_id]));
      }
      seg_counter += (long long)batch.size();        // H9: count segments processed by this consumer
      if(cb && !cb_lock.exchange(true)){              // best-effort throttled live cb
        auto now = std::chrono::steady_clock::now();
        // One snapshot drives BOTH the 150ms bar cb AND the 1s log (coherent by build).
        BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load();
        bp.audio_seconds_done=(double)samples_done.load()/16000.0;
        bp.total_audio_decoded=(double)decoded_samples.load()/16000.0;
        bp.cpu_segs=cpu_segs.load(); bp.gpu_segs=gpu_segs.load();   // G14: live split
        bp.segs_done=segs_done.load(); bp.segs_total=segs_total.load();
        if(consumer_last_cb == std::chrono::steady_clock::time_point{} ||
           std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_cb).count() >= 150){
          consumer_last_cb = now;
          fill_files(bp);
          cb(bp);
        }
        // STEP 6: live density line on a SEPARATE ~1s cadence, still inside cb_lock so the
        // timestamp is serialized exactly like consumer_last_cb. Hetero variant appends the
        // CPU/GPU split from bp.cpu_segs/gpu_segs. MUST NOT move outside the spinlock.
        if(consumer_last_log == std::chrono::steady_clock::time_point{} ||
           std::chrono::duration_cast<std::chrono::milliseconds>(now - consumer_last_log).count() >= 1000){
          consumer_last_log = now;
          double el = std::chrono::duration<double>(now - t0).count();
          double x  = el > 0 ? bp.audio_seconds_done / el : 0.0;
          double eta = bp.segs_done > 0
            ? el * (double)(bp.segs_total - bp.segs_done) / (double)bp.segs_done : 0.0;
          std::string line = u8"转写中 " + std::to_string(bp.segs_done) + "/" + std::to_string(bp.segs_total)
                           + u8" 段 · " + fmt1(x) + "x · " + u8"剩余~" + mmss(eta);
          long long tot = bp.cpu_segs + bp.gpu_segs;
          if(tot > 0){
            int cpu_pct = (int)(bp.cpu_segs * 100 / tot);
            line += u8" · CPU " + std::to_string(cpu_pct) + "%/GPU " + std::to_string(100 - cpu_pct) + "%";
          }
          log_info(line);
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
  // Qwen3-ASR self-punctuates (LLM decoder); skip the CT-transformer pass to avoid doubled
  // marks (。。 / ，，). Same detection + rationale as the single path. AED/CTC keep punctuating.
  const bool self_punct = !cfg.qwen3_encoder.empty();
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
      Transcript tr;
      tr.segments = merge_file_tokens(std::move(toks), cfg.merge_gap, cfg.merge_max_dur);
      for(auto& seg : tr.segments){ seg.text = self_punct ? seg.text : punct.add(seg.text); tr.full_text += seg.text; }
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
    const EngineConfig& cfg, const AutoTune& tune, ProgressCb cb, CancelToken* cancel,
    const std::vector<double>* file_full_seconds){
  if(tune.provider == Provider::Hetero)
    return transcribe_batch_files_hetero(inputs, cfg, tune, cb, cancel, file_full_seconds);
  return transcribe_batch_files_single(inputs, cfg, tune, cb, cancel, file_full_seconds);
}
}  // namespace suji
