#include "doctest/doctest.h"
#include "core/batch_engine.h"
#include "core/cancel.h"
#include "core/config.h"
#include <thread>
#include <chrono>
#include <algorithm>
using namespace suji;

TEST_CASE("cancel returns promptly without deadlock" * doctest::timeout(60)) {
    std::string md = SUJI_DEFAULT_MODELS_DIR;
    std::string m  = md + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
    std::string w  = m  + "test_wavs/";
    EngineConfig c;
    c.ffmpeg_path = SUJI_DEFAULT_FFMPEG;
    c.asr_model   = m + "model.int8.onnx";
    c.tokens      = m + "tokens.txt";
    c.vad_model   = md + "/silero_vad.onnx";
    c.punct_model = md + "/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";
    std::vector<std::string> inputs = {
        w + "0.wav", w + "1.wav", w + "2.wav",
        w + "3.wav", w + "4-tianjin.wav", w + "5-henan.wav"
    };
    AutoTune tune;
    tune.provider      = Provider::Cpu;
    tune.batch         = 4;
    tune.in_flight_files = 2;
    tune.num_threads   = 4;

    CancelToken cancel;
    std::vector<FileResult> res;

    std::thread worker([&]{
        res = transcribe_batch_files(inputs, c, tune, nullptr, &cancel);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // let it start
    cancel.cancel();
    worker.join();  // must return (timeout fails if deadlock)

    CHECK(res.size() == inputs.size());  // returned one result per input
    // cancellation must have had an effect: at least one file marked cancelled
    CHECK(std::any_of(res.begin(), res.end(),
          [](const FileResult& r){ return !r.ok && r.err == "cancelled"; }));
}
