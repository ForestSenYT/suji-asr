#include "gui/engine_worker.h"

#include "core/batch_engine.h"
#include "core/hardware.h"
#include "core/config.h"
#include "core/log.h"
#include "core/media_decode.h"
#include "core/output/writer_facade.h"
#include "core/paths.h"
#include "core/resume.h"

#include <QDir>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace suji {

namespace {
// UTF-8-safe stem: find the last path separator and the last dot using ASCII
// bytes only. UTF-8 is ASCII-transparent, so multibyte names (e.g. Chinese) are
// preserved. std::filesystem's narrow path would mis-read UTF-8 as the system
// ANSI codepage and mangle them (测试视频 -> 娴嬭瘯瑙嗛).
static std::string stem(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? p : p.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot != 0) name = name.substr(0, dot);
    return name;
}
} // namespace

void EngineWorker::run(QStringList inputs, QString outDir, QString provider,
                       bool srt, bool vtt, bool json, bool md,
                       int batchOverride, int inFlightOverride)
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
    { auto mp = default_model_paths();
      c.ffmpeg_path = ffmpeg_path();
      c.asr_model   = mp.asr_model;
      c.tokens      = mp.tokens;
      c.vad_model   = mp.vad_model;
      c.punct_model = mp.punct_model; }

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
    if      (prov == "cpu")    tune.provider = Provider::Cpu;
    else if (prov == "cuda")   tune.provider = Provider::Cuda;
    else if (prov == "hetero") {
        // Mirror batch_main.cpp H4 logic: validate hardware, fill tunables or fall back.
        bool gpu_ok = hw.has_cuda_gpu && hw.gpu_free_mb >= 3000 && hw.cuda_runtime_available;
        bool cpu_ok = hw.cpu_threads >= 12;
        if (!(gpu_ok && cpu_ok)) {
            log_info("hetero unavailable (need CUDA GPU + >=12 cores), falling back");
            tune.provider = hw.has_cuda_gpu ? Provider::Cuda : Provider::Cpu;
        } else {
            fill_hetero(tune, hw);
        }
    }

    // Crash-safe CUDA/Hetero: only run on GPU when the CUDA DLL dir is known.
    if (tune.provider == Provider::Cuda || tune.provider == Provider::Hetero) {
        c.cuda_dll_dir = hw.cuda_dll_dir;
        if (c.cuda_dll_dir.empty()) {
            log_info("CUDA runtime not found, falling back to CPU");
            tune.provider = Provider::Cpu;
        }
    }

    // Recompute CPU tunables after any fallback.
    if (tune.provider == Provider::Cpu) {
        tune.num_threads = std::max(4, hw.cpu_threads);
        tune.batch       = std::min(4, std::max(1, hw.cpu_threads / 4));
    }

    // G12: apply GUI batch/in-flight overrides (0 = auto, leave decide() value as-is)
    if (batchOverride > 0) {
        tune.batch     = batchOverride;
        tune.gpu_batch = batchOverride;
    }
    if (inFlightOverride > 0)
        tune.in_flight_files = inFlightOverride;

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
    // G7: Resume partition — skip files whose outputs are already complete.
    // Mirrors suji_batch's resume logic in batch_main.cpp.
    // ------------------------------------------------------------------
    std::set<std::string> used_bases;  // G6: shared across resumed + transcribed outputs

    std::vector<std::string> todo;
    // skipped_inputs preserves order so we can emit fileResult for them later
    std::vector<std::string> skipped_inputs;
    for (const std::string& f : vec) {
        std::string base_candidate = outDirStd + "/" + stem(f);
        // Compute the unique base for this file (G6 dedup across the whole batch).
        std::string base = base_candidate;
        int n = 2;
        while (used_bases.count(base)) { base = base_candidate + "_" + std::to_string(n++); }

        if (transcript_complete(base, c)) {
            // Reserve the base for this skipped file so later inputs can't collide with it.
            used_bases.insert(base);
            skipped_inputs.push_back(f);
            log_info("resume: skip (done) " + f);
        } else {
            // Don't reserve the base here; the results loop will reserve it after
            // transcription succeeds (prevents false collisions if transcription fails).
            todo.push_back(f);
        }
    }
    if (!skipped_inputs.empty())
        log_info("resume: " + std::to_string(skipped_inputs.size()) + " file(s) already complete, skipping");

    // ------------------------------------------------------------------
    // Run transcription
    // ------------------------------------------------------------------
    auto t0 = std::chrono::steady_clock::now();

    // Probe total audio duration for determinate progress (only todo files)
    double totalAudio = 0.0;
    for (const std::string& f : todo) {
        if (cancel_.is_cancelled()) break;   // abort probing fast on cancel
        double d = probe_duration_seconds(ffprobe_path(), f);
        if (d > 0.0) totalAudio += d;
    }
    log_info("total audio to transcribe: " + std::to_string(static_cast<int>(totalAudio)) + "s");

    emit started(QString::fromUtf8(provider_str(tune.provider)),
                 static_cast<int>(vec.size()));

    auto results = transcribe_batch_files(
        todo, c, tune,
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

    // G7: Emit "already done" results for skipped (resumed) files.
    for (const std::string& f : skipped_inputs) {
        ++okCount;
        emit fileResult(
            QString::fromUtf8(f.c_str()),
            true,
            /*segments=*/0,    // outputs already exist; segment count not re-read
            QString()
        );
    }

    for (const FileResult& r : results) {
        bool wasCancelled = (!r.ok && r.err == "cancelled");

        if (r.ok) {
            // G6: deduplicate output base when two inputs share the same filename.
            // used_bases was pre-seeded during resume partition, so collisions are
            // detected even between todo-batch outputs and resumed-file bases.
            std::string base_candidate = outDirStd + "/" + stem(r.input);
            std::string base = base_candidate;
            int nn = 2;
            while (used_bases.count(base)) { base = base_candidate + "_" + std::to_string(nn++); }
            used_bases.insert(base);
            if (base != base_candidate)
                log_err("output stem collision for '" + r.input + "' -> writing as " + base);
            if (write_outputs(r.transcript, base, c, stem(r.input))) {
                ++okCount;
                emit fileResult(
                    QString::fromUtf8(r.input.c_str()),
                    true,
                    static_cast<int>(r.transcript.segments.size()),
                    QString()
                );
            } else {
                ++failedCount;
                log_err("write failed: " + base);
                emit fileResult(
                    QString::fromUtf8(r.input.c_str()),
                    false,
                    0,
                    QStringLiteral("write failed")
                );
            }
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
