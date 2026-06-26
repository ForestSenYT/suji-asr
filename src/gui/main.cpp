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
        QObject::connect(&w, &suji::EngineWorker::progress,
            [](int d, int t, double a, double tot, long long cs, long long gs) {
            std::printf("progress: %d/%d audio=%.1f total=%.1f cpu=%lld gpu=%lld\n",
                        d, t, a, tot, cs, gs); std::fflush(stdout);
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

    // ── Modern dark theme ────────────────────────────────────────────
    // One accent (#4a9eff), dark background family, clean Segoe UI.
    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(QStringLiteral(R"(
/* ── Base / window ──────────────────────────────────────────────── */
QWidget {
    background-color: #1e1f22;
    color: #e6e6e6;
    font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
    font-size: 9pt;
}
QMainWindow {
    background-color: #1e1f22;
}

/* ── Tool bar ────────────────────────────────────────────────────── */
QToolBar {
    background-color: #2b2d31;
    border-bottom: 1px solid #3a3d43;
    padding: 3px 6px;
    spacing: 4px;
}
QToolBar::separator {
    background: #3a3d43;
    width: 1px;
    margin: 4px 6px;
}

/* ── Toolbar buttons (AddFiles / AddFolder / Clear / OutDir) ─────── */
QToolBar QPushButton {
    background-color: #3a3d43;
    color: #e6e6e6;
    border: 1px solid #4a4d55;
    border-radius: 5px;
    padding: 4px 12px;
    min-width: 60px;
}
QToolBar QPushButton:hover {
    background-color: #484c56;
    border-color: #6b7080;
}
QToolBar QPushButton:pressed {
    background-color: #2b2d31;
}

/* ── Labels ──────────────────────────────────────────────────────── */
QLabel {
    background: transparent;
    color: #b0b3bb;
}

/* ── Table ───────────────────────────────────────────────────────── */
QTableView {
    background-color: #1e1f22;
    alternate-background-color: #252628;
    gridline-color: #2d2f33;
    selection-background-color: #1e3a5f;
    selection-color: #e6e6e6;
    border: 1px solid #3a3d43;
    border-radius: 4px;
    outline: none;
}
QTableView::item {
    padding: 3px 6px;
    min-height: 26px;
    border: none;
}
QTableView::item:selected {
    background-color: #1e3a5f;
    color: #e6e6e6;
}
QTableView::item:focus {
    background-color: #214570;
    outline: none;
}
QHeaderView::section {
    background-color: #2b2d31;
    color: #c0c3cb;
    font-weight: 600;
    padding: 5px 8px;
    border: none;
    border-right: 1px solid #3a3d43;
    border-bottom: 1px solid #3a3d43;
}
QHeaderView::section:last {
    border-right: none;
}

/* ── Splitter ────────────────────────────────────────────────────── */
QSplitter::handle {
    background-color: #3a3d43;
    height: 2px;
}
QSplitter::handle:hover {
    background-color: #4a9eff;
}

/* ── Log panel ───────────────────────────────────────────────────── */
QPlainTextEdit {
    background-color: #16171a;
    color: #b0b3bb;
    border: 1px solid #3a3d43;
    border-radius: 4px;
    font-family: "Cascadia Mono", "Consolas", monospace;
    font-size: 8pt;
    selection-background-color: #1e3a5f;
}

/* ── Bottom panel background ─────────────────────────────────────── */
QWidget#bottomWidget {
    background-color: #2b2d31;
    border-top: 1px solid #3a3d43;
}

/* ── Combo box ───────────────────────────────────────────────────── */
QComboBox {
    background-color: #2b2d31;
    color: #e6e6e6;
    border: 1px solid #4a4d55;
    border-radius: 5px;
    padding: 3px 8px;
    min-width: 72px;
}
QComboBox:hover {
    border-color: #6b7080;
}
QComboBox:focus {
    border-color: #4a9eff;
}
QComboBox::drop-down {
    border: none;
    width: 18px;
}
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #b0b3bb;
    margin-right: 4px;
}
QComboBox QAbstractItemView {
    background-color: #2b2d31;
    color: #e6e6e6;
    selection-background-color: #1e3a5f;
    border: 1px solid #4a9eff;
    outline: none;
}

/* ── Spin boxes ──────────────────────────────────────────────────── */
QSpinBox {
    background-color: #2b2d31;
    color: #e6e6e6;
    border: 1px solid #4a4d55;
    border-radius: 5px;
    padding: 3px 6px;
    min-width: 54px;
}
QSpinBox:hover {
    border-color: #6b7080;
}
QSpinBox:focus {
    border-color: #4a9eff;
}
QSpinBox::up-button, QSpinBox::down-button {
    background-color: #3a3d43;
    border: none;
    width: 16px;
    border-radius: 3px;
}
QSpinBox::up-button:hover, QSpinBox::down-button:hover {
    background-color: #4a9eff;
}

/* ── Check boxes ─────────────────────────────────────────────────── */
QCheckBox {
    color: #e6e6e6;
    spacing: 5px;
    background: transparent;
}
QCheckBox::indicator {
    width: 14px;
    height: 14px;
    border: 1px solid #4a4d55;
    border-radius: 3px;
    background-color: #2b2d31;
}
QCheckBox::indicator:checked {
    background-color: #4a9eff;
    border-color: #4a9eff;
    image: none;
}
QCheckBox::indicator:hover {
    border-color: #4a9eff;
}

/* ── Progress bar ────────────────────────────────────────────────── */
QProgressBar {
    background-color: #2b2d31;
    border: 1px solid #3a3d43;
    border-radius: 9px;
    height: 18px;
    text-align: center;
    color: #e6e6e6;
    font-size: 8pt;
}
QProgressBar::chunk {
    background-color: #4a9eff;
    border-radius: 8px;
}

/* ── Primary action button (Start / 开始) ────────────────────────── */
QPushButton#btnStart {
    background-color: #4a9eff;
    color: #ffffff;
    border: none;
    border-radius: 6px;
    padding: 5px 24px;
    font-weight: 600;
    min-width: 80px;
}
QPushButton#btnStart:hover {
    background-color: #3a8ef0;
}
QPushButton#btnStart:pressed {
    background-color: #2a7de0;
}
QPushButton#btnStart:disabled {
    background-color: #2b2d31;
    color: #555860;
    border: 1px solid #3a3d43;
}

/* ── Secondary action button (Cancel / 取消) ─────────────────────── */
QPushButton#btnCancel {
    background-color: transparent;
    color: #b0b3bb;
    border: 1px solid #4a4d55;
    border-radius: 6px;
    padding: 5px 24px;
    min-width: 80px;
}
QPushButton#btnCancel:hover {
    background-color: #3a3d43;
    color: #e6e6e6;
    border-color: #6b7080;
}
QPushButton#btnCancel:pressed {
    background-color: #2b2d31;
}
QPushButton#btnCancel:disabled {
    color: #444750;
    border-color: #3a3d43;
}

/* ── Scroll bars ─────────────────────────────────────────────────── */
QScrollBar:vertical {
    background: #1e1f22;
    width: 8px;
    margin: 0;
    border-radius: 4px;
}
QScrollBar::handle:vertical {
    background: #3a3d43;
    border-radius: 4px;
    min-height: 20px;
}
QScrollBar::handle:vertical:hover {
    background: #4a9eff;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0;
}
QScrollBar:horizontal {
    background: #1e1f22;
    height: 8px;
    border-radius: 4px;
}
QScrollBar::handle:horizontal {
    background: #3a3d43;
    border-radius: 4px;
    min-width: 20px;
}
QScrollBar::handle:horizontal:hover {
    background: #4a9eff;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    width: 0;
}

/* ── Tool tips ───────────────────────────────────────────────────── */
QToolTip {
    background-color: #2b2d31;
    color: #e6e6e6;
    border: 1px solid #4a9eff;
    border-radius: 4px;
    padding: 3px 6px;
}
)"));

    suji::MainWindow win;
    win.show();
    return app.exec();
}
