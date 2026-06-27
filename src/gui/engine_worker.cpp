#include "gui/engine_worker.h"

#include "core/batch_engine.h"
#include "core/hardware.h"
#include "core/config.h"
#include "core/log.h"
#include "core/media_decode.h"
#include "core/output/writer_facade.h"
#include "core/paths.h"
#include "core/resume.h"
#include "core/utf8_file.h"

#include <QDir>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// UTF-8-safe parent directory: everything up to (not incl.) the last path
// separator. Operates on the raw UTF-8 bytes (ASCII-transparent) so Chinese
// path components survive — routing through std::filesystem's narrow path would
// re-interpret UTF-8 as the system ANSI codepage and mangle them. Returns "."
// when the input has no separator (a bare filename in the cwd).
static std::string parent_dir(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? std::string(".") : p.substr(0, slash);
}

// Check whether a directory is writable: create it (noop if it exists; fails if
// a file component blocks the path), then create+delete a tiny probe file.
// UTF-8-safe: uses write_utf8_no_bom (wide API on Windows) + wide DeleteFileW.
static bool dir_is_writable(const std::string& dir_utf8) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir_utf8), ec);
    if (ec) return false;

    std::string probe = dir_utf8 + "/.suji_writable_probe";
    if (!suji::write_utf8_no_bom(probe, "")) return false;

#ifdef _WIN32
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, probe.data(), (int)probe.size(), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, probe.data(), (int)probe.size(), w.data(), n);
        DeleteFileW(w.c_str());
    }
#else
    std::remove(probe.c_str());
#endif
    return true;
}

// Return the effective output directory for desired_dir, falling back to
// fallback_dir when desired_dir is not writable.  Result is cached in
// dir_cache (keyed by desired_dir) so each distinct dir is probed once per
// batch and the redirect log fires once per dir.
static std::string effective_dir_cached(const std::string& desired_dir,
                                        const std::string& fallback_dir,
                                        std::map<std::string, std::string>& dir_cache)
{
    auto it = dir_cache.find(desired_dir);
    if (it != dir_cache.end()) return it->second;

    std::string effective;
    if (dir_is_writable(desired_dir)) {
        effective = desired_dir;
    } else {
        // Ensure fallback exists, then redirect.
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(fallback_dir), ec);
        log_info(u8"源目录不可写(" + desired_dir + u8"/) — 已改存到 " + fallback_dir + "/");
        effective = fallback_dir;
    }
    dir_cache[desired_dir] = effective;
    return effective;
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
            // Pass the input file count so the thread split reclaims idle producer
            // cores when only a few files are queued.
            fill_hetero(tune, hw, static_cast<int>(inputs.size()));
        }
    }

    // --- Auto-prefer the fp16 FireRedASR AED model on GPU (mirrors batch_main.cpp) ---
    // When the fp16-AED model is present AND a working CUDA GPU is available AND the
    // user didn't force a CPU/hetero provider, use fp16-AED on CUDA: it's ~9.2x faster
    // than int8-CTC on GPU AND more accurate (int8-CTC leaks "< sil >" markers). So the
    // GUI "just works" with the better model. fp16 is GPU-only (crashes on the CPU EP),
    // so the crash-safe CUDA-DLL fallback below (empty cuda_dll_dir -> CPU) still guards
    // against a missing CUDA runtime. The GUI has no manual --asr-encoder override.
    if (prov != "cpu" && prov != "hetero" &&
        hw.has_cuda_gpu && hw.cuda_runtime_available) {
        AedModel aed = discover_fp16_aed();
        if (aed.ok()) {
            tune.provider = Provider::Cuda;
            c.asr_encoder = aed.encoder;
            c.asr_decoder = aed.decoder;
            c.tokens      = aed.tokens;
            c.cuda_dll_dir = hw.cuda_dll_dir;
            log_info("using fp16 AED model on GPU (faster + more accurate than int8)");
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
        // Qwen3 data-parallel CPU consumers (no-op for the GUI's fp16-AED/CTC models;
        // qwen3_encoder is empty there). Keeps parity with batch_main.cpp's CPU path.
        set_qwen3_cpu_consumers(tune, c, hw.cpu_threads);
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
    // Resolve output location. Empty outDir => write next to each SOURCE file
    // (matches the "（与源文件相同）" toolbar label); a chosen dir is created up
    // front. Per-file bases are computed via resolve_base() below.
    // ------------------------------------------------------------------
    const std::string outDirStd = outDir.isEmpty() ? std::string() : outDir.toUtf8().constData();
    if (!outDirStd.empty()) {
        QDir().mkpath(outDir);
        std::filesystem::create_directories(outDirStd);
    }

    // Fallback output directory: Documents/suji-转写 (guaranteed writable even
    // when the source dir is read-protected, e.g. Program Files).
    const std::string fallback_dir = []() -> std::string {
        QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (docs.isEmpty())
            docs = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        docs += u8"/suji-\xe8\xbd\xac\xe5\x86\x99";  // /suji-转写
        QDir().mkpath(docs);
        return docs.toUtf8().constData();
    }();
    // Per-batch dir writability cache: desired_dir -> effective_dir.
    // Avoids probing the same dir twice and logs the redirect only once per dir.
    std::map<std::string, std::string> dir_cache;

    // ------------------------------------------------------------------
    // G7: Resume partition — skip files whose outputs are already complete.
    // Mirrors suji_batch's resume logic in batch_main.cpp.
    // ------------------------------------------------------------------
    std::set<std::string> used_bases;  // G6: shared across resumed + transcribed outputs

    std::vector<std::string> todo;
    // skipped_inputs preserves order so we can emit fileResult for them later
    std::vector<std::string> skipped_inputs;
    for (const std::string& f : vec) {
        // Desired dir: next-to-source (empty outDirStd) or user-chosen dir.
        // effective_dir_cached falls back to Documents/suji-转写 if not writable.
        std::string desired_dir = outDirStd.empty() ? parent_dir(f) : outDirStd;
        std::string eff_dir     = effective_dir_cached(desired_dir, fallback_dir, dir_cache);
        std::string base_candidate = eff_dir + "/" + stem(f);
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
        [this, totalAudio, &todo](const BatchProgress& b) {
            emit progress(b.files_done, b.files_total, b.audio_seconds_done, totalAudio,
                          b.cpu_segs, b.gpu_segs, b.segs_done, b.segs_total);
            // PER-FILE progress: map each FilePstat.file_index -> the engine-input
            // PATH (todo[index]; resume already filtered, so todo IS the engine's
            // inputs vector) and emit one fileProgress per file. QString/int are
            // registered metatypes, so the queued cross-thread delivery is safe.
            for (const FilePstat& fp : b.files) {
                if (fp.file_index < 0 || fp.file_index >= (int)todo.size()) continue;
                int percent = (fp.segs_total > 0)
                    ? std::min(100, (int)(100 * fp.segs_done / fp.segs_total))
                    : 0;
                emit fileProgress(QString::fromUtf8(todo[fp.file_index].c_str()),
                                  percent, (int)fp.segs_done, (int)fp.segs_total);
            }
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
            // effective_dir_cached is already warm from the resume partition loop.
            std::string desired_dir    = outDirStd.empty() ? parent_dir(r.input) : outDirStd;
            std::string eff_dir        = effective_dir_cached(desired_dir, fallback_dir, dir_cache);
            std::string base_candidate = eff_dir + "/" + stem(r.input);
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
