#include "gui/main_window.h"
#include "gui/engine_worker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
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
                QStringList{ QString::fromLocal8Bit(argv[2]) },
                QStringLiteral("build/gui_selftest"),
                QStringLiteral("cpu"),
                true, true, true, true
            );
        });

        app.exec();
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
