#include "gui/engine_worker.h"

#include "core/batch_engine.h"
#include "core/hardware.h"
#include "core/config.h"
#include "core/log.h"
#include "core/media_decode.h"
#include "core/output/writer_facade.h"
#include "core/paths.h"

#include <QDir>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace suji {

namespace {
static std::string stem(const std::string& p) {
    return std::filesystem::path(p).stem().string();
}
} // namespace

void EngineWorker::run(QStringList inputs, QString outDir, QString provider,
                       bool srt, bool vtt, bool json, bool md)
{
    // Re-entrancy guard: reject concurrent calls (e.g. double-click Start)
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
        return;

    // RAII guard to reset running_ on all exit paths
    struct RunGuard {
        std::atomic<bool>& flag;
        ~RunGuard() { flag.store(false); }
    } runGuard{running_};

    cancel_.cancelled.store(false);

    // ------------------------------------------------------------------
    // Build EngineConfig from relocatable paths (app-relative, dev fallback)
    // ------------------------------------------------------------------
    EngineConfig c;
    std::string mdl = models_dir();
    std::string m   = mdl + "/sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/";
    c.ffmpeg_path = ffmpeg_path();
    c.asr_model   = m   + "model.int8.onnx";
    c.tokens      = m   + "tokens.txt";
    c.vad_model   = mdl + "/silero_vad.onnx";
    c.punct_model = mdl + "/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx";

    c.out_srt  = srt;
    c.out_vtt  = vtt;
    c.out_json = json;
    c.out_md   = md;

    // ------------------------------------------------------------------
    // Hardware probe + adaptive provider selection
    // ------------------------------------------------------------------
    HardwareInfo hw   = probe_hardware();
    AutoTune     tune = decide(hw, c);

    // Honor the UI provider combo: "auto" keeps decide()'s choice.
    std::string prov = provider.toStdString();
    if      (prov == "cpu")  tune.provider = Provider::Cpu;
    else if (prov == "cuda") tune.provider = Provider::Cuda;

    // Crash-safe CUDA: only run on GPU when the CUDA DLL dir is known.
    if (tune.provider == Provider::Cuda) {
        c.cuda_dll_dir = hw.cuda_dll_dir;          // auto-detected path
        if (c.cuda_dll_dir.empty()) { tune.provider = Provider::Cpu; }   // no CUDA runtime -> CPU
    }

    // Recompute CPU tunables after any fallback.
    if (tune.provider == Provider::Cpu) {
        tune.num_threads = std::max(4, hw.cpu_threads);
        tune.batch       = std::min(4, std::max(1, hw.cpu_threads / 4));
    }

    log_info(std::string("GUI engine: provider=") + provider_str(tune.provider));

    c.provider    = tune.provider;
    c.num_threads = tune.num_threads;

    // ------------------------------------------------------------------
    // Convert QStringList -> std::vector<std::string> (UTF-8 for Chinese)
    // ------------------------------------------------------------------
    std::vector<std::string> vec;
    vec.reserve(static_cast<size_t>(inputs.size()));
    for (const QString& p : inputs)
        vec.emplace_back(p.toUtf8().constData());

    // ------------------------------------------------------------------
    // Ensure output directory exists
    // ------------------------------------------------------------------
    std::string outDirStd;
    if (outDir.isEmpty()) {
        outDirStd = "out";
        outDir    = QStringLiteral("out");
    } else {
        outDirStd = outDir.toUtf8().constData();
    }
    QDir().mkpath(outDir);
    std::filesystem::create_directories(outDirStd);

    // ------------------------------------------------------------------
    // Run transcription
    // ------------------------------------------------------------------
    auto t0 = std::chrono::steady_clock::now();

    // Probe total audio duration for determinate progress
    double totalAudio = 0.0;
    for (const std::string& f : vec) {
        double d = probe_duration_seconds(ffprobe_path(), f);
        if (d > 0.0) totalAudio += d;
    }
    log_info("total audio to transcribe: " + std::to_string(static_cast<int>(totalAudio)) + "s");

    emit started(QString::fromUtf8(provider_str(tune.provider)),
                 static_cast<int>(vec.size()));

    auto results = transcribe_batch_files(
        vec, c, tune,
        [this, totalAudio](const BatchProgress& b) {
            emit progress(b.files_done, b.files_total, b.audio_seconds_done, totalAudio);
        },
        &cancel_
    );

    double wallSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    // ------------------------------------------------------------------
    // Write outputs + tally
    // ------------------------------------------------------------------
    int okCount        = 0;
    int failedCount    = 0;
    int cancelledCount = 0;

    for (const FileResult& r : results) {
        bool wasCancelled = (!r.ok && r.err == "cancelled");

        if (r.ok) {
            ++okCount;
            std::string base = outDirStd + "/" + stem(r.input);
            write_outputs(r.transcript, base, c, stem(r.input));
            emit fileResult(
                QString::fromUtf8(r.input.c_str()),
                true,
                static_cast<int>(r.transcript.segments.size()),
                QString()
            );
        } else if (wasCancelled) {
            ++cancelledCount;
            emit fileResult(
                QString::fromUtf8(r.input.c_str()),
                false,
                0,
                QStringLiteral("cancelled")
            );
        } else {
            ++failedCount;
            emit fileResult(
                QString::fromUtf8(r.input.c_str()),
                false,
                0,
                QString::fromUtf8(r.err.c_str())
            );
        }
    }

    emit finished(okCount, failedCount, cancelledCount, wallSec);
}

void EngineWorker::requestCancel()
{
    cancel_.cancel();
}

} // namespace suji
