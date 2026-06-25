#include "gui/main_window.h"
#include "gui/engine_worker.h"

#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMetaObject>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QThread>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <chrono>

namespace suji {

// ---------------------------------------------------------------------------
// Media extension filter (same set as suji_batch)
// ---------------------------------------------------------------------------
static const QStringList& mediaExtensions()
{
    static const QStringList exts = {
        QStringLiteral("mp4"),  QStringLiteral("mkv"),  QStringLiteral("mov"),
        QStringLiteral("flv"),  QStringLiteral("avi"),  QStringLiteral("webm"),
        QStringLiteral("ts"),   QStringLiteral("m4a"),  QStringLiteral("mp3"),
        QStringLiteral("wav"),  QStringLiteral("flac"), QStringLiteral("aac"),
        QStringLiteral("ogg"),  QStringLiteral("opus")
    };
    return exts;
}

bool MainWindow::isMediaFile(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return mediaExtensions().contains(ext);
}

// ---------------------------------------------------------------------------
// Column indices
// ---------------------------------------------------------------------------
enum Col { ColFile = 0, ColStatus = 1, ColSegs = 2, ColErr = 3, ColCount = 4 };

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("suji 批量转写"));
    resize(1000, 660);
    setAcceptDrops(true);

    // -----------------------------------------------------------------------
    // Central widget + root layout
    // -----------------------------------------------------------------------
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    // -----------------------------------------------------------------------
    // Top toolbar row
    // -----------------------------------------------------------------------
    auto* toolBar = new QToolBar(tr("工具"), this);
    toolBar->setMovable(false);
    addToolBar(Qt::TopToolBarArea, toolBar);

    auto* btnAddFiles = new QPushButton(tr("添加文件…"), toolBar);
    auto* btnAddFolder = new QPushButton(tr("添加文件夹…"), toolBar);
    auto* btnClear = new QPushButton(tr("清空"), toolBar);
    auto* btnOutDir = new QPushButton(tr("输出目录…"), toolBar);
    m_outDirLabel = new QLabel(tr("（与源文件相同）"), toolBar);
    m_outDirLabel->setMinimumWidth(200);

    toolBar->addWidget(btnAddFiles);
    toolBar->addWidget(btnAddFolder);
    toolBar->addWidget(btnClear);
    toolBar->addSeparator();
    toolBar->addWidget(m_outDirLabel);
    toolBar->addWidget(btnOutDir);

    connect(btnAddFiles,  &QPushButton::clicked, this, &MainWindow::onAddFiles);
    connect(btnAddFolder, &QPushButton::clicked, this, &MainWindow::onAddFolder);
    connect(btnClear,     &QPushButton::clicked, this, &MainWindow::onClearList);
    connect(btnOutDir,    &QPushButton::clicked, this, &MainWindow::onChooseOutputDir);

    // -----------------------------------------------------------------------
    // Center: QTableView + model
    // -----------------------------------------------------------------------
    m_model = new QStandardItemModel(0, ColCount, this);
    m_model->setHorizontalHeaderLabels({tr("文件"), tr("状态"), tr("段数"), tr("错误")});

    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setSectionResizeMode(ColFile,   QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColStatus, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColSegs,   QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColErr,    QHeaderView::Fixed);
    m_table->setColumnWidth(ColStatus, 80);
    m_table->setColumnWidth(ColSegs,   60);
    m_table->setColumnWidth(ColErr,    200);
    m_table->verticalHeader()->setVisible(false);

    rootLayout->addWidget(m_table, /*stretch=*/1);

    // -----------------------------------------------------------------------
    // Bottom panel
    // -----------------------------------------------------------------------
    auto* bottomWidget = new QWidget(this);
    auto* bottomLayout = new QVBoxLayout(bottomWidget);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(4);

    // Settings row: provider + format checkboxes
    auto* settingsRow = new QHBoxLayout;
    settingsRow->setSpacing(12);

    auto* providerLabel = new QLabel(tr("推理后端:"), bottomWidget);
    m_provider = new QComboBox(bottomWidget);
    m_provider->addItem(QStringLiteral("auto"));
    m_provider->addItem(QStringLiteral("cpu"));
    m_provider->addItem(QStringLiteral("cuda"));

    m_chkSrt  = new QCheckBox(QStringLiteral("SRT"),  bottomWidget);
    m_chkVtt  = new QCheckBox(QStringLiteral("VTT"),  bottomWidget);
    m_chkJson = new QCheckBox(QStringLiteral("JSON"), bottomWidget);
    m_chkMd   = new QCheckBox(QStringLiteral("MD"),   bottomWidget);
    m_chkSrt->setChecked(true);
    m_chkVtt->setChecked(true);
    m_chkJson->setChecked(true);
    m_chkMd->setChecked(true);

    settingsRow->addWidget(providerLabel);
    settingsRow->addWidget(m_provider);
    settingsRow->addSpacing(16);
    settingsRow->addWidget(new QLabel(tr("输出格式:"), bottomWidget));
    settingsRow->addWidget(m_chkSrt);
    settingsRow->addWidget(m_chkVtt);
    settingsRow->addWidget(m_chkJson);
    settingsRow->addWidget(m_chkMd);
    settingsRow->addStretch();

    // Progress row
    auto* progressRow = new QHBoxLayout;
    m_progress = new QProgressBar(bottomWidget);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_statusLabel = new QLabel(tr("就绪"), bottomWidget);
    m_statusLabel->setMinimumWidth(260);
    progressRow->addWidget(m_progress, /*stretch=*/1);
    progressRow->addWidget(m_statusLabel);

    // Action row: Start / Cancel
    auto* actionRow = new QHBoxLayout;
    actionRow->addStretch();
    m_btnStart  = new QPushButton(tr("开始"), bottomWidget);
    m_btnCancel = new QPushButton(tr("取消"), bottomWidget);
    m_btnCancel->setEnabled(false);
    m_btnStart->setMinimumWidth(80);
    m_btnCancel->setMinimumWidth(80);
    actionRow->addWidget(m_btnStart);
    actionRow->addWidget(m_btnCancel);

    connect(m_btnStart,  &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnCancel, &QPushButton::clicked, this, &MainWindow::onCancel);

    bottomLayout->addLayout(settingsRow);
    bottomLayout->addLayout(progressRow);
    bottomLayout->addLayout(actionRow);

    rootLayout->addWidget(bottomWidget);

    // ------------------------------------------------------------------
    // Worker thread setup (Task 4)
    // ------------------------------------------------------------------
    workerThread_ = new QThread(this);
    worker_       = new EngineWorker();
    worker_->moveToThread(workerThread_);

    connect(worker_, &EngineWorker::progress,
            this,    &MainWindow::onWorkerProgress);
    connect(worker_, &EngineWorker::fileResult,
            this,    &MainWindow::onWorkerFileResult);
    connect(worker_, &EngineWorker::finished,
            this,    &MainWindow::onWorkerFinished);

    // Clean up worker object when thread finishes
    connect(workerThread_, &QThread::finished,
            worker_,       &QObject::deleteLater);

    workerThread_->start();
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
MainWindow::~MainWindow()
{
    workerThread_->quit();
    workerThread_->wait();
}

// ---------------------------------------------------------------------------
// Slot implementations
// ---------------------------------------------------------------------------
void MainWindow::onAddFiles()
{
    const QString filter = tr("媒体文件 (*.mp4 *.mkv *.mov *.flv *.avi *.webm *.ts "
                              "*.m4a *.mp3 *.wav *.flac *.aac *.ogg *.opus);;所有文件 (*)");
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("选择媒体文件"), QString(), filter);
    for (const QString& f : files)
        addInputFile(f);
}

void MainWindow::onAddFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择文件夹"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty())
        addFolder(dir);
}

void MainWindow::onClearList()
{
    m_model->removeRows(0, m_model->rowCount());
}

void MainWindow::onChooseOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择输出目录"), m_outputDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        m_outputDir = dir;
        m_outDirLabel->setText(dir);
    }
}

void MainWindow::onStart()
{
    const QStringList files = inputFiles();
    if (files.isEmpty()) {
        setStatusText(tr("请先添加文件"));
        return;
    }

    // Determine output directory: use chosen dir or default "out"
    QString outDir = m_outputDir;
    if (outDir.isEmpty()) {
        outDir = QStringLiteral("out");
        m_outDirLabel->setText(outDir);
    }
    QDir().mkpath(outDir);

    // Reset UI state
    m_btnStart->setEnabled(false);
    m_btnCancel->setEnabled(true);
    m_progress->setValue(0);

    // Reset all row statuses to "待处理"
    for (int r = 0; r < m_model->rowCount(); ++r) {
        setRowStatus(r, tr("待处理"));
        if (auto* item = m_model->item(r, ColSegs)) item->setText(QString());
        setRowError(r, QString());
    }
    setStatusText(tr("正在初始化…"));

    m_startTime = std::chrono::steady_clock::now();

    // Invoke worker on its thread via queued connection
    QMetaObject::invokeMethod(
        worker_, "run",
        Qt::QueuedConnection,
        Q_ARG(QStringList, files),
        Q_ARG(QString,     outDir),
        Q_ARG(QString,     provider()),
        Q_ARG(bool,        wantSrt()),
        Q_ARG(bool,        wantVtt()),
        Q_ARG(bool,        wantJson()),
        Q_ARG(bool,        wantMd())
    );
}

void MainWindow::onCancel()
{
    setStatusText(tr("正在取消…"));
    QMetaObject::invokeMethod(worker_, "requestCancel", Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Worker signal handlers
// ---------------------------------------------------------------------------
void MainWindow::onWorkerProgress(int filesDone, int filesTotal, double audioSec)
{
    if (filesTotal > 0)
        m_progress->setValue(100 * filesDone / filesTotal);

    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_startTime).count();
    double throughput = (elapsed > 0.0) ? (audioSec / elapsed) : 0.0;

    setStatusText(tr("处理中 %1/%2  %3 倍速")
        .arg(filesDone)
        .arg(filesTotal)
        .arg(throughput, 0, 'f', 1));
}

void MainWindow::onWorkerFileResult(QString path, bool ok, int segments, QString err)
{
    // Find the row whose Qt::UserRole data matches the path
    const int n = m_model->rowCount();
    for (int r = 0; r < n; ++r) {
        if (m_model->item(r, ColFile)->data(Qt::UserRole).toString() == path) {
            if (ok) {
                setRowStatus(r, tr("完成"));
                setRowSegments(r, segments);
            } else if (err == QStringLiteral("cancelled")) {
                setRowStatus(r, tr("取消"));
            } else {
                setRowStatus(r, tr("失败"));
                setRowError(r, err);
            }
            return;
        }
    }
}

void MainWindow::onWorkerFinished(int ok, int failed, int cancelled, double wallSec)
{
    m_progress->setValue(100);
    m_btnStart->setEnabled(true);
    m_btnCancel->setEnabled(false);

    double elapsed = (wallSec > 0.0) ? wallSec : 1.0;
    setStatusText(
        tr("完成: %1 成功  %2 失败  %3 取消  %4 秒")
            .arg(ok)
            .arg(failed)
            .arg(cancelled)
            .arg(elapsed, 0, 'f', 1)
    );
}

// ---------------------------------------------------------------------------
// Drag-and-drop
// ---------------------------------------------------------------------------
void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
    else
        QMainWindow::dragEnterEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl& url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString localPath = url.toLocalFile();
        const QFileInfo fi(localPath);
        if (fi.isDir())
            addFolder(localPath);
        else if (isMediaFile(localPath))
            addInputFile(localPath);
    }
    event->acceptProposedAction();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Signal cancel and wait for worker thread to drain before closing
    if (worker_)
        QMetaObject::invokeMethod(worker_, "requestCancel", Qt::QueuedConnection);
    workerThread_->quit();
    workerThread_->wait(5000); // 5 second timeout
    QMainWindow::closeEvent(event);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
void MainWindow::addInputFile(const QString& path)
{
    // Dedup: check Qt::UserRole data in col-0 items
    const int n = m_model->rowCount();
    for (int r = 0; r < n; ++r) {
        if (m_model->item(r, ColFile)->data(Qt::UserRole).toString() == path)
            return;
    }

    const QString displayName = QFileInfo(path).fileName();
    auto* itemFile   = new QStandardItem(displayName);
    itemFile->setData(path, Qt::UserRole);   // full path stored here
    itemFile->setToolTip(path);

    auto* itemStatus = new QStandardItem(tr("待处理"));
    auto* itemSegs   = new QStandardItem(QString());
    auto* itemErr    = new QStandardItem(QString());

    m_model->appendRow({itemFile, itemStatus, itemSegs, itemErr});
}

void MainWindow::addFolder(const QString& dir)
{
    const QDir d(dir);
    const QFileInfoList entries = d.entryInfoList(
        QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
        QDir::Name);
    for (const QFileInfo& fi : entries) {
        if (isMediaFile(fi.absoluteFilePath()))
            addInputFile(fi.absoluteFilePath());
    }
}

// ---------------------------------------------------------------------------
// Getters for Task 4
// ---------------------------------------------------------------------------
QStringList MainWindow::inputFiles() const
{
    QStringList result;
    result.reserve(m_model->rowCount());
    for (int r = 0; r < m_model->rowCount(); ++r)
        result << m_model->item(r, ColFile)->data(Qt::UserRole).toString();
    return result;
}

QString MainWindow::outputDir() const  { return m_outputDir; }
QString MainWindow::provider() const   { return m_provider->currentText(); }
bool    MainWindow::wantSrt()  const   { return m_chkSrt->isChecked(); }
bool    MainWindow::wantVtt()  const   { return m_chkVtt->isChecked(); }
bool    MainWindow::wantJson() const   { return m_chkJson->isChecked(); }
bool    MainWindow::wantMd()   const   { return m_chkMd->isChecked(); }

// ---------------------------------------------------------------------------
// Task 4 update helpers
// ---------------------------------------------------------------------------
void MainWindow::setProgress(int value)
{
    m_progress->setValue(value);
}

void MainWindow::setStatusText(const QString& text)
{
    m_statusLabel->setText(text);
}

void MainWindow::setRowStatus(int row, const QString& status)
{
    if (auto* item = m_model->item(row, ColStatus))
        item->setText(status);
}

void MainWindow::setRowSegments(int row, int segs)
{
    if (auto* item = m_model->item(row, ColSegs))
        item->setText(QString::number(segs));
}

void MainWindow::setRowError(int row, const QString& err)
{
    if (auto* item = m_model->item(row, ColErr))
        item->setText(err);
}

} // namespace suji
