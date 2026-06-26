#include "gui/main_window.h"
#include "gui/engine_worker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

int main(int argc, char** argv)
{
    // Windows: char** argv is ANSI-mangled for non-ASCII paths. Fetch the real
    // UTF-16 command line so --selftest* can receive Chinese/Unicode file paths
    // (the normal GUI gets paths from Qt as UTF-16, so it is unaffected).
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    const QString selfArg2 = (wargv && wargc >= 3) ? QString::fromWCharArray(wargv[2])
                           : (argc >= 3 ? QString::fromUtf8(argv[2]) : QString());
    const QString selfArg3 = (wargv && wargc >= 4) ? QString::fromWCharArray(wargv[3]) : QString();
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
                QStringList{ selfArg2 },  // Fix 4: UTF-8 for consistency
                QStringLiteral("build/gui_selftest"),
                QStringLiteral("auto"),
                true, true, true, true,
                0, 0  // G12: batchOverride=auto, inFlightOverride=auto
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
        QString wavPath = selfArg2;
        QStringList inputs;
        for (int i = 0; i < 6; ++i)
            inputs << wavPath;

        // Enqueue run() on the worker thread via queued connection (same as GUI onStart)
        QMetaObject::invokeMethod(
            &w, "run",
            Qt::QueuedConnection,
            Q_ARG(QStringList, inputs),
            Q_ARG(QString, QStringLiteral("build/gui_selftest_cancel")),
            Q_ARG(QString, QStringLiteral("auto")),
            Q_ARG(bool, true),
            Q_ARG(bool, false),
            Q_ARG(bool, false),
            Q_ARG(bool, false),
            Q_ARG(int, 0),   // G12: batchOverride=auto
            Q_ARG(int, 0)    // G12: inFlightOverride=auto
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
    // Headless thread-path test: --selftest-thread <file>
    // Worker on a QThread (like the real GUI), ALL FOUR signals connected,
    // runs to COMPLETION (no cancel). Covers the gap left by --selftest
    // (main thread) and --selftest-cancel (cancelled before finalize):
    // worker-thread finalize + queued delivery of every signal.
    // ------------------------------------------------------------------
    if (argc >= 3 && std::string(argv[1]) == "--selftest-thread") {
        QCoreApplication app(argc, argv);
        QThread workerThread;
        suji::EngineWorker w;
        w.moveToThread(&workerThread);
        workerThread.start();
        int rc = 1;
        QObject::connect(&w, &suji::EngineWorker::started, [](QString p, int n) {
            std::printf("started: provider=%s files=%d\n", p.toUtf8().constData(), n);
            std::fflush(stdout);
        });
        QObject::connect(&w, &suji::EngineWorker::progress, [](int d, int t, double a, double tot) {
            std::printf("progress: %d/%d audio=%.1f total=%.1f\n", d, t, a, tot); std::fflush(stdout);
        });
        QObject::connect(&w, &suji::EngineWorker::fileResult, [](QString path, bool ok, int segs, QString err) {
            std::printf("fileResult: ok=%d segs=%d err=%s\n", ok, segs, err.toUtf8().constData());
            std::fflush(stdout);
        });
        QObject::connect(&w, &suji::EngineWorker::finished, [&](int ok, int f, int c, double wall) {
            std::printf("finished: ok=%d failed=%d cancelled=%d wall=%.1f\n", ok, f, c, wall);
            std::fflush(stdout);
            rc = (ok > 0) ? 0 : 1;
            app.quit();
        });
        QMetaObject::invokeMethod(&w, "run", Qt::QueuedConnection,
            Q_ARG(QStringList, QStringList{ selfArg2 }),
            Q_ARG(QString, QStringLiteral("build/st_thread")),
            Q_ARG(QString, QStringLiteral("auto")),
            Q_ARG(bool, true), Q_ARG(bool, true), Q_ARG(bool, true), Q_ARG(bool, true),
            Q_ARG(int, 0), Q_ARG(int, 0));  // G12: batchOverride=auto, inFlightOverride=auto
        app.exec();
        workerThread.quit();
        workerThread.wait();
        return rc;
    }

    // ------------------------------------------------------------------
    // Headless FULL-GUI test: --selftest-gui <file>
    // Drives the REAL MainWindow (offscreen): add file + onStart, poll the
    // row "状态" + bottom status text every 1s. Reproduces "stuck at 待处理".
    // ------------------------------------------------------------------
    if (argc >= 3 && std::string(argv[1]) == "--selftest-gui") {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication app(argc, argv);
        suji::MainWindow win;
        win.show();

        const QString file = selfArg2;
        QTimer::singleShot(0, [&win, file]() { win.testStart(file); });

        // optional: --selftest-gui <file> <cancelSeconds> -> click Cancel mid-run
        if (!selfArg3.isEmpty()) {
            int cancelMs = int(selfArg3.toDouble() * 1000.0);
            QTimer::singleShot(cancelMs, [&win]() {
                std::printf(">>> Cancel clicked now\n"); std::fflush(stdout);
                win.testCancel();
            });
        }

        int ticks = 0;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, [&]() {
            const QString rs = win.testRowStatus(0);
            std::printf("[t=%2ds] row0=\"%s\" prog=%d status=\"%s\"\n",
                        ticks, rs.toUtf8().constData(),
                        win.testProgressValue(),
                        win.testStatusText().toUtf8().constData());
            std::fflush(stdout);
            ++ticks;
            const bool done = (rs == QString::fromUtf8("完成")
                            || rs == QString::fromUtf8("失败")
                            || rs == QString::fromUtf8("取消"));
            if (done) {
                std::printf("--- log panel ---\n%s\n--- end log ---\n",
                    win.testLogText().toUtf8().constData());
                std::fflush(stdout);
            }
            if (done || ticks > 45) app.quit();
        });
        poll.start(1000);
        return app.exec();
    }

    // ------------------------------------------------------------------
    // Normal GUI path
    // ------------------------------------------------------------------
    QApplication app(argc, argv);
    // G11: must be set before any QSettings are constructed
    QApplication::setOrganizationName(QStringLiteral("suji"));
    QApplication::setApplicationName(QStringLiteral("suji-asr"));
    suji::MainWindow win;
    win.show();
    return app.exec();
}
