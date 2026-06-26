// H3 — heterogeneous engine integration tests (real models).
//
//  * graceful degradation: a bad cuda_dll_dir forces gpu_asr.ok()==false, the
//    engine must still complete every file via the CPU-only consumer.
//  * end-to-end hetero: run transcribe_batch_files() with a Hetero AutoTune on
//    3 real short clips; assert all outputs produced, segs>0, no crash. If the
//    CUDA runtime is absent the CPU-only consumer carries the batch (still ok).

#include "doctest/doctest.h"
#include "core/batch_engine.h"
#include "core/config.h"
#include "core/hardware.h"
#include "core/paths.h"
#include "core/cancel.h"
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>

using namespace suji;

static std::string md(){ return SUJI_DEFAULT_MODELS_DIR; }
static EngineConfig hcfg(){
  EngineConfig c;
  std::string m = md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
  c.ffmpeg_path = SUJI_DEFAULT_FFMPEG;
  c.asr_model   = m+"model.int8.onnx";
  c.tokens      = m+"tokens.txt";
  c.vad_model   = md()+"/silero_vad.onnx";
  c.punct_model = md()+"/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
  return c;
}
static std::vector<std::string> clips(){
  std::string w = md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  return { w+"0.wav", w+"1.wav", w+"2.wav" };
}
// Hetero tunables mirroring decide()'s hetero branch for a 12+ core dev box.
static AutoTune htune(){
  AutoTune t;
  t.provider=Provider::Hetero; t.hetero=true;
  t.in_flight_files=4; t.cpu_asr_threads=11; t.cpu_batch=4; t.gpu_batch=30;
  t.num_threads=t.cpu_asr_threads; t.batch=t.gpu_batch;
  return t;
}

TEST_CASE("hetero: graceful degradation to CPU-only on bad cuda_dll_dir"
          * doctest::timeout(300)){
  EngineConfig c = hcfg();
  c.cuda_dll_dir = "Z:/definitely/not/a/cuda/dir";   // forces gpu_asr.ok()==false
  auto res = transcribe_batch_files(clips(), c, htune());
  REQUIRE(res.size()==3);
  for(auto& r : res){
    CHECK(r.ok);                                    // CPU consumer completed every file
    CHECK_FALSE(r.transcript.full_text.empty());
    CHECK(r.transcript.segments.size() > 0);
  }
}

TEST_CASE("hetero: cancel returns promptly, no truncated transcript reported ok"
          * doctest::timeout(120)){
  EngineConfig c = hcfg();
  c.cuda_dll_dir = cuda_dll_dir();
  std::string w = md()+"/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/test_wavs/";
  std::vector<std::string> inputs = {
    w+"0.wav", w+"1.wav", w+"2.wav", w+"3.wav", w+"4-tianjin.wav", w+"5-henan.wav"
  };

  CancelToken cancel;
  std::vector<FileResult> res;
  std::thread worker([&]{ res = transcribe_batch_files(inputs, c, htune(), nullptr, &cancel); });
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // let producers/consumers start
  cancel.cancel();
  worker.join();                                                  // both consumers must join (timeout fails on hang)

  REQUIRE(res.size()==inputs.size());
  // R6: cancellation had an effect AND every reported-ok file has real content
  // (a truncated transcript is classified cancelled, never silently ok-but-empty).
  CHECK(std::any_of(res.begin(), res.end(),
        [](const FileResult& r){ return !r.ok && r.err=="cancelled"; }));
  for(auto& r : res){
    if(r.ok) CHECK_FALSE(r.transcript.full_text.empty());        // no ok-but-empty truncation
  }
}

TEST_CASE("hetero: end-to-end on 3 real clips (cuda auto-detected)"
          * doctest::timeout(420)){
  EngineConfig c = hcfg();
  c.cuda_dll_dir = cuda_dll_dir();                  // "" if no CUDA runtime -> CPU-only consumer
  bool have_cuda = !c.cuda_dll_dir.empty();
  MESSAGE("cuda_dll_dir = '" << c.cuda_dll_dir << "' (have_cuda=" << have_cuda << ")");

  auto res = transcribe_batch_files(clips(), c, htune());
  REQUIRE(res.size()==3);
  size_t total_segs=0;
  for(size_t i=0;i<res.size();++i){
    CHECK(res[i].ok);
    CHECK_FALSE(res[i].transcript.full_text.empty());
    CHECK(res[i].transcript.segments.size() > 0);
    total_segs += res[i].transcript.segments.size();
    MESSAGE("file[" << i << "] ok=" << res[i].ok
            << " segs=" << res[i].transcript.segments.size()
            << " text='" << res[i].transcript.full_text << "'");
  }
  CHECK(total_segs > 0);
  std::string mode = have_cuda ? std::string("dual-consumer (CPU+CUDA)")
                               : std::string("CPU-only fallback (no CUDA runtime)");
  MESSAGE("HETERO E2E: 3 files, total_segs=" << total_segs
          << ", provider=hetero, no crash — " << mode);
}
