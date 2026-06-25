#include "gui/main_window.h"
#include "gui/engine_worker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    // ------------------------------------------------------------------
    // Headless self-test: --selftest <wavfile>
    // Verifies the EngineWorker pipeline without a GUI / platform plugin.
    // Usage: suji_gui.exe --selftest path/to/audio.wav
    // ------------------------------------------------------------------
    if (argc >= 3 && std::string(argv[1]) == "--selftest") {
        QCoreApplication app(argc, argv);

        suji::EngineWorker w;
        int rc = 1;

        QObject::connect(
            &w, &suji::EngineWorker::finished,
            [&](int ok, int failed, int cancelled, double wall) {
                std::printf("selftest: ok=%d failed=%d cancelled=%d wall=%.1fs\n",
                            ok, failed, cancelled, wall);
                rc = (ok > 0) ? 0 : 1;
                app.quit();
            }
        );

        QTimer::singleShot(0, [&]() {
            w.run(
                QStringList{ QString::fromUtf8(argv[2]) },  // Fix 4: UTF-8 for consistency
                QStringLiteral("build/gui_selftest"),
                QStringLiteral("cpu"),
                true, true, true, true
            );
        });

        app.exec();
        return rc;
    }

    // ------------------------------------------------------------------
    // Headless cancel verification: --selftest-cancel <wavfile>
    // Proves that direct requestCancel() from another thread reaches the
    // blocked worker's run() and cancels the batch mid-run.
    // Usage: suji_gui.exe --selftest-cancel path/to/audio.wav
    // ------------------------------------------------------------------
    if (argc >= 3 && std::string(argv[1]) == "--selftest-cancel") {
        QCoreApplication app(argc, argv);

        // Worker lives on a separate thread — exactly like the real GUI —
        // so run() blocks the worker's event loop for the whole batch.
        QThread workerThread;
        suji::EngineWorker w;
        w.moveToThread(&workerThread);
        workerThread.start();

        int rc = 1;

        QObject::connect(
            &w, &suji::EngineWorker::finished,
            [&](int ok, int failed, int cancelled, double wall) {
                std::printf("selftest-cancel: ok=%d failed=%d cancelled=%d wall=%.1fs\n",
                            ok, failed, cancelled, wall);
                // Pass: at least one file was cancelled AND the batch finished well
                // before all 6 files could have been processed (proves mid-run cancel).
                rc = (cancelled > 0 && wall < 25.0) ? 0 : 1;
                app.quit();
            }
        );

        // Build a 6-file batch from the same wav — gives enough runtime to cancel
        QString wavPath = QString::fromUtf8(argv[2]);
        QStringList inputs;
        for (int i = 0; i < 6; ++i)
            inputs << wavPath;

        // Enqueue run() on the worker thread via queued connection (same as GUI onStart)
        QMetaObject::invokeMethod(
            &w, "run",
            Qt::QueuedConnection,
            Q_ARG(QStringList, inputs),
            Q_ARG(QString, QStringLiteral("build/gui_selftest_cancel")),
            Q_ARG(QString, QStringLiteral("cpu")),
            Q_ARG(bool, true),
            Q_ARG(bool, false),
            Q_ARG(bool, false),
            Q_ARG(bool, false)
        );

        // After 1500ms, call requestCancel() DIRECTLY from this (main) thread —
        // this is exactly what the fixed onCancel() does in the real GUI.
        QTimer::singleShot(1500, [&w]() {
            w.requestCancel();  // direct atomic store — reaches blocked worker immediately
        });

        app.exec();

        workerThread.quit();
        workerThread.wait();
        return rc;
    }

    // ------------------------------------------------------------------
    // Normal GUI path
    // ------------------------------------------------------------------
    QApplication app(argc, argv);
    suji::MainWindow win;
    win.show();
    return app.exec();
}
