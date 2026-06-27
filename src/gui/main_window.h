#pragma once
#include <QMainWindow>
#include <QProgressBar>
#include <QStringList>
#include <QString>
#include <QIcon>
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
class QTextEdit;
class QTimer;
class QPoint;
class QResizeEvent;

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
    int         mode() const;       // 转写模式: 0=Qwen3(准确度) 1=AED(速度) 2=CTC(词级字幕)
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

    // Pure helper: build a colored HTML log line (for unit testing without Qt event loop).
    // Exported as static so tests can call it directly.
    static QString logLineHtml(const QString& level, const QString& msg, const QString& timestamp);

    // Pure helper (E): resolve the folder a completed row's outputs were written to,
    // given the row's source PATH and the chosen output dir (empty => next-to-source).
    // Mirrors EngineWorker's desired_dir = chosenDir.isEmpty() ? parentDir(path) : chosenDir.
    // Exported as static so tests can call it without a widget. Does NOT model the
    // not-writable -> Documents fallback (the row stores its effective dir at finish);
    // this returns the *likely* dir for the "打开输出文件夹" action.
    static QString rowOutputDir(const QString& path, const QString& chosenDir);

    // Pure helper (D): the app-wide QSS stylesheet string. Exported so a test can
    // assert it is non-empty and main.cpp can apply it via qApp->setStyleSheet().
    static QString themeStyleSheet();

    // Pure helper (D): build the programmatic app icon (rounded-rect + waveform "S")
    // so there is an intentional icon without an external asset.
    static QIcon makeAppIcon();

    // Headless test hooks (offscreen --selftest-gui): drive the real interactive path
    void testStart(const QString& file);     // addInputFile + onStart
    // Screenshot/demo hook: add a row WITHOUT starting the worker (so --screenshot
    // can populate the table with representative rows + progress for a still image).
    void testAddRow(const QString& path);
    QString testRowStatus(int row) const;     // current "状态" cell text
    QString testStatusText() const;           // bottom status label text
    QString testLogText() const;             // current log panel text (plain text extraction)
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

    // E: row interactions.
    void onTableContextMenu(const QPoint& pos);   // 移除 / 打开输出文件夹 / 打开转写结果
    void onTableDoubleClicked(const QModelIndex& index);  // 完成行 -> 打开输出文件夹

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;  // E: clear drag highlight
    void dropEvent(QDropEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;        // D: keep empty-state overlay centered
    bool eventFilter(QObject* obj, QEvent* event) override;  // re-center hint on viewport's own resize

private:
    void addInputFile(const QString& path);
    void addFolder(const QString& dir);
    static bool isMediaFile(const QString& path);
    void loadSettings();   // G11: restore persisted GUI state
    void saveSettings();   // G11: persist GUI state (called in closeEvent + onStart)

    // E/D interaction helpers
    void setDragHighlight(bool on);          // toggle accent drop-zone border on the table
    void updateEmptyState();                 // show/hide the centered "拖入…" hint over the table
    void openRowOutputFolder(int row);       // open the row's output dir in the file explorer
    void openRowTranscript(int row);         // open the row's produced .srt/.md if it exists
    QString rowEffectiveOutputDir(int row) const;  // stored effective dir if known, else rowOutputDir()

    // Widgets
    QTableView*         m_table       = nullptr;
    QTextEdit*          m_log         = nullptr;
    QStandardItemModel* m_model       = nullptr;
    QProgressBar*       m_progress    = nullptr;
    QLabel*             m_statusLabel = nullptr;
    QComboBox*          m_provider    = nullptr;
    QComboBox*          m_mode        = nullptr;  // 转写模式: 准确度(Qwen3)/速度(AED)/词级字幕(CTC)
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

    // E: optional "open first output folder when the batch finishes" toggle.
    QCheckBox*          m_chkOpenOnFinish = nullptr;
    // D: centered muted hint shown over the table when it has 0 rows.
    QLabel*             m_emptyHint   = nullptr;
    // E: the chosen output dir captured at the moment a run starts, so the
    // row context menu / double-click resolves the SAME dir the worker used
    // even if the user edits the field afterwards. Empty => next-to-source.
    QString             m_runOutputDir;
    // E: first effective output dir seen this run (for 完成后打开输出文件夹).
    QString             m_firstOutputDir;

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
