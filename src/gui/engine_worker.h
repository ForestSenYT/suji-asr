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
             bool srt, bool vtt, bool json, bool md);
    void requestCancel(); // thread-safe: direct atomic store, callable from any thread

signals:
    void started(QString provider, int filesTotal);
    void progress(int filesDone, int filesTotal, double audioSec, double totalAudioSec);
    void fileResult(QString path, bool ok, int segments, QString err);
    void finished(int ok, int failed, int cancelled, double wallSec);

private:
    CancelToken cancel_;
    std::atomic<bool> running_{false}; // re-entrancy guard
};

} // namespace suji
