#pragma once
#include <QObject>
#include <QStringList>
#include <QString>
#include "core/cancel.h"

namespace suji {

class EngineWorker : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

public slots:
    void run(QStringList inputs, QString outDir, QString provider,
             bool srt, bool vtt, bool json, bool md);
    void requestCancel(); // thread-safe: sets the cancel token

signals:
    void progress(int filesDone, int filesTotal, double audioSec);
    void fileResult(QString path, bool ok, int segments, QString err);
    void finished(int ok, int failed, int cancelled, double wallSec);

private:
    CancelToken cancel_;
};

} // namespace suji
