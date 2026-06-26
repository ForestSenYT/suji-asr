#pragma once
#include <QObject>
#include <QStringList>
#include <QString>
#include <atomic>
#include "core/cancel.h"

namespace suji {

class EngineWorker : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

public slots:
    void run(QStringList inputs, QString outDir, QString provider,
             bool srt, bool vtt, bool json, bool md,
             int batchOverride = 0, int inFlightOverride = 0);
    void requestCancel(); // thread-safe: direct atomic store, callable from any thread

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
};

} // namespace suji
