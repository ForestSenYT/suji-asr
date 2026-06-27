#pragma once
#include <QObject>
#include <QStringList>
#include <QString>
#include <atomic>
#include <mutex>
#include "core/cancel.h"
#include "core/hardware.h"

namespace suji {

// Transcription mode (transcription-quality preset). Drives the model + the
// recommended provider in EngineWorker::run(). The int values are persisted in
// QSettings and passed across the queued cross-thread run() invocation, so keep
// them stable: 0=Qwen3 (default, most accurate), 1=AED (fp16, fastest), 2=CTC
// (int8, per-token timestamps for word-level subtitles).
enum class Mode { Qwen3 = 0, Aed = 1, Ctc = 2 };

class EngineWorker : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

public slots:
    void run(QStringList inputs, QString outDir, QString provider,
             bool srt, bool vtt, bool json, bool md,
             int batchOverride = 0, int inFlightOverride = 0,
             int mode = 0 /*Mode::Qwen3*/);
    void requestCancel(); // thread-safe: direct atomic store, callable from any thread

    // STARTUP (STEP 10): cache a hardware probe so the FIRST Start doesn't stall on
    // nvidia-smi (WaitForSingleObject INFINITE). MainWindow kicks setCachedHardware() from a
    // one-shot background thread after show(); run() reads it via cachedHardware() IF ready,
    // otherwise falls back to an inline probe_hardware() so an early Start never regresses.
    // Thread-safety: a mutex guards the HardwareInfo + an atomic ready flag, so the background
    // writer and the run()-thread reader never race on a half-written struct.
    void setCachedHardware(const HardwareInfo& hw);   // called from the background probe thread
    bool cachedHardware(HardwareInfo& out) const;     // true (+fills out) if the probe finished

signals:
    void started(QString provider, int filesTotal);
    // cpuSegs/gpuSegs: live hetero CPU/GPU segment split (0/0 for single-provider paths).
    // segsDone/segsTotal: segment-based progress; drives the determinate % bar to 100%.
    void progress(int filesDone, int filesTotal, double audioSec, double totalAudioSec,
                  long long cpuSegs, long long gpuSegs,
                  long long segsDone, long long segsTotal);
    // PER-FILE progress ("每个视频分开"): one emission per file per progress callback,
    // keyed by the original input PATH. The engine's file_index ≠ GUI table row when
    // resume filters files, so the worker maps index→path before emitting. percent = 0
    // when segsTotal == 0 (file still decoding/VAD with nothing queued yet).
    void fileProgress(QString path, int percent, int segsDone, int segsTotal);
    void fileResult(QString path, bool ok, int segments, QString err);
    void finished(int ok, int failed, int cancelled, double wallSec);

private:
    CancelToken cancel_;
    std::atomic<bool> running_{false}; // re-entrancy guard

    // STEP 10: cached hardware probe (written by the background probe, read by run()).
    mutable std::mutex     hw_mu_;
    HardwareInfo           hw_cached_;
    std::atomic<bool>      hw_ready_{false};
};

} // namespace suji
