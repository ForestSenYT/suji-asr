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
    if(tr.segments.empty() && toks.empty()){ /* no speech detected -- still ok=true, empty */ }
    results[i].transcript = std::move(tr);
    files_done++;
    if(cb){ BatchProgress bp; bp.files_total=N; bp.files_done=files_done.load(); bp.audio_seconds_done=(double)samples_done.load()/16000.0; cb(bp); }
  }
  return results;
}
}
