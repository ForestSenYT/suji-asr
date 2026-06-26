#pragma once
#include <QMainWindow>
#include <QProgressBar>
#include <QStringList>
#include <chrono>

class QTableView;
class QStandardItemModel;
class QProgressBar;
class QLabel;
class QComboBox;
class QCheckBox;
class QPushButton;
class QSpinBox;
class QThread;
class QPlainTextEdit;
class QTimer;

namespace suji {

class EngineWorker;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Getters for Task 4 wiring
    QStringList inputFiles() const;
    QString     outputDir() const;
    QString     provider() const;   // "auto" | "cpu" | "cuda" | "hetero"
    bool        wantSrt()  const;
    bool        wantVtt()  const;
    bool        wantJson() const;
    bool        wantMd()   const;
    int         batchOverride()    const;  // 0 = auto
    int         inFlightOverride() const;  // 0 = auto

    // Called by Task 4 to update progress / status
    void setProgress(int value);
    void setStatusText(const QString& text);
    void setRowStatus(int row, const QString& status);
    void setRowSegments(int row, int segs);
    void setRowError(int row, const QString& err);
    void setRowProgress(int row, int percent);   // per-file progress bar (-1 = unset)

    // Headless test hooks (offscreen --selftest-gui): drive the real interactive path
    void testStart(const QString& file);     // addInputFile + onStart
    QString testRowStatus(int row) const;     // current "状态" cell text
    QString testStatusText() const;           // bottom status label text
    QString testLogText() const;             // current log panel text
    int testProgressValue() const { return m_progress ? m_progress->value() : -1; }
    int testRowProgress(int row) const;       // current per-file progress % (-1 = unset)
    void testCancel();                        // invoke the real onCancel()

public slots:
    void onAddFiles();
    void onAddFolder();
    void onClearList();
    void onChooseOutputDir();
    void onStart();
    void onCancel();

    // Worker signal handlers
    void onWorkerStarted(QString provider, int filesTotal);
    void onWorkerProgress(int filesDone, int filesTotal, double audioSec, double totalAudioSec,
                          long long cpuSegs, long long gpuSegs,
                          long long segsDone, long long segsTotal);
    // PER-FILE progress: find the row by matching the stored path (Qt::UserRole on the
    // 文件 column, like onWorkerFileResult) and set its 进度/段数 cells.
    void onFileProgress(QString path, int percent, int segsDone, int segsTotal);
    void onWorkerFileResult(QString path, bool ok, int segments, QString err);
    void onWorkerFinished(int ok, int failed, int cancelled, double wallSec);

    void appendLogLine(QString level, QString msg);

    // Fires every 1s while a job runs so the status line (elapsed/ETA/throughput) keeps
    // moving even when engine progress callbacks are sparse. (G14)
    void onSecondTick();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void addInputFile(const QString& path);
    void addFolder(const QString& dir);
    static bool isMediaFile(const QString& path);
    void loadSettings();   // G11: restore persisted GUI state
    void saveSettings();   // G11: persist GUI state (called in closeEvent + onStart)

    // Widgets
    QTableView*         m_table       = nullptr;
    QPlainTextEdit*     m_log         = nullptr;
    QStandardItemModel* m_model       = nullptr;
    QProgressBar*       m_progress    = nullptr;
    QLabel*             m_statusLabel = nullptr;
    QComboBox*          m_provider    = nullptr;
    QCheckBox*          m_chkSrt      = nullptr;
    QCheckBox*          m_chkVtt      = nullptr;
    QCheckBox*          m_chkJson     = nullptr;
    QCheckBox*          m_chkMd       = nullptr;
    QSpinBox*           m_spnBatch    = nullptr;  // G12: batch override (0 = auto)
    QSpinBox*           m_spnInFlight = nullptr;  // G12: in-flight override (0 = auto)
    QPushButton*        m_btnStart    = nullptr;
    QPushButton*        m_btnCancel   = nullptr;
    QLabel*             m_outDirLabel = nullptr;
    QString             m_outputDir;

    // Worker thread (Task 4)
    QThread*       workerThread_ = nullptr;
    EngineWorker*  worker_       = nullptr;

    // G14: per-second status repaint. Drives onSecondTick() while a job runs so the
    // elapsed/ETA/throughput line keeps moving between sparse engine callbacks.
    QTimer*        m_tick        = nullptr;

    // Timing for throughput display
    std::chrono::steady_clock::time_point m_startTime;
    // Transcription-phase timing: 倍速 excludes the init/decode/VAD startup so it
    // reflects the real transcription speed, not a startup-dragged average.
    std::chrono::steady_clock::time_point m_transStartTime;
    double m_transStartAudio = -1.0;   // -1 => transcription not started yet this run

    // ------------------------------------------------------------------
    // G14: live-feedback state. Latest snapshot from onWorkerProgress so the
    // 1s tick can recompute elapsed/ETA/throughput without waiting for the
    // next (possibly seconds-away) engine callback.
    // ------------------------------------------------------------------
    bool      m_jobRunning      = false;  // true between onWorkerStarted and onWorkerFinished
    int       m_lastFilesDone   = 0;
    int       m_lastFilesTotal  = 0;
    double    m_lastAudioSec     = 0.0;   // VAD-speech seconds transcribed so far
    double    m_lastTotalAudio   = 0.0;   // total speech to transcribe (for %)
    long long m_lastCpuSegs      = 0;
    long long m_lastGpuSegs      = 0;
    long long m_lastSegsDone     = 0;     // segment-based progress: segments routed so far
    long long m_lastSegsTotal    = 0;     // segment-based progress: segments queued so far
    int       m_lastPct          = 0;     // last computed % (held between ticks)

    // EMA of the instantaneous transcription rate (倍速). Smooths out the noisy,
    // misleadingly-low early window so the figure settles on the true rate fast.
    double                                m_emaRate     = 0.0;   // 0 => unseeded
    double                                m_prevAudioSec = -1.0; // previous callback's audioSec
    std::chrono::steady_clock::time_point m_prevRateTime;        // previous callback's wall time

    // Re-render the bottom status line from the current snapshot (called by both
    // onWorkerProgress and the 1s tick so the two never disagree).
    void renderStatusLine();
};

} // namespace suji
