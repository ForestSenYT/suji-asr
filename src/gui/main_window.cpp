#include "gui/main_window.h"
#include "gui/engine_worker.h"
#include "core/log.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLinearGradient>
#include <QMenu>
#include <QMetaObject>
#include <QClipboard>
#include <QMimeData>
#include <QPen>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShortcut>
#include <QTextEdit>
#include <QTime>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionProgressBar>
#include <QPainter>
#include <QPainterPath>
#include <QTableView>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QDirIterator>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
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
// Column indices.  进度 (per-file progress bar) sits right after 状态 so the
// user reads file -> status -> live bar left-to-right ("每个视频分开").
// ---------------------------------------------------------------------------
enum Col { ColFile = 0, ColStatus = 1, ColProgress = 2, ColSegs = 3, ColErr = 4, ColCount = 5 };

// Per-row progress percent is stored on the 进度 cell's item via this role.
// -1 / absent = not started (the delegate paints an empty track).
static constexpr int ProgressRole = Qt::UserRole + 1;

// ---------------------------------------------------------------------------
// Theme accent palette (D). Kept here as the single source of truth so the
// custom-painted per-row progress bar matches the QSS accent (the QSS string
// can't reach a delegate that paints with QPainter directly).
// ---------------------------------------------------------------------------
namespace theme {
    static const QColor kAccent  (0x4c, 0x9a, 0xff);  // calm teal-blue
    static const QColor kAccentHi(0x63, 0xa9, 0xff);  // accent highlight (gradient top)
    static const QColor kTrack   (0x2a, 0x2e, 0x3a);  // progress track / header
    static const QColor kBorder  (0x33, 0x38, 0x45);  // subtle border
    static const QColor kText     (0xe7, 0xe9, 0xee);  // primary text
}

// ---------------------------------------------------------------------------
// ProgressBarDelegate — paints a rounded accent-filled progress bar (with "%"
// text) in the 进度 column, reflecting the row's ProgressRole value. We draw it
// by hand (rounded track + accent gradient chunk) rather than via the platform
// style so it honors the theme accent regardless of QSS reach.
// Percent < 0 / unset -> paint nothing but the default cell background.
// ---------------------------------------------------------------------------
namespace {
class ProgressBarDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        // Let the base class draw selection/alternating-row background first.
        QStyledItemDelegate::paint(painter, option, index);

        bool ok = false;
        const int pct = index.data(ProgressRole).toInt(&ok);
        if (!ok || pct < 0)
            return;   // not started: leave the (already-painted) empty cell

        const int clamped = std::clamp(pct, 0, 100);
        const QRectF track = QRectF(option.rect).adjusted(4, 5, -4, -5);
        const qreal radius = track.height() / 2.0;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        // Track (subtle, rounded).
        QPainterPath trackPath;
        trackPath.addRoundedRect(track, radius, radius);
        painter->fillPath(trackPath, theme::kTrack);
        painter->setPen(QPen(theme::kBorder, 1.0));
        painter->drawPath(trackPath);

        // Accent chunk (vertical gradient for a little depth).
        if (clamped > 0) {
            QRectF fill = track;
            fill.setWidth(track.width() * clamped / 100.0);
            if (fill.width() < radius * 2.0)  // keep the rounded cap visible at low %
                fill.setWidth(std::min(track.width(), radius * 2.0));
            QPainterPath fillPath;
            fillPath.addRoundedRect(fill, radius, radius);
            QLinearGradient g(fill.topLeft(), fill.bottomLeft());
            g.setColorAt(0.0, theme::kAccentHi);
            g.setColorAt(1.0, theme::kAccent);
            painter->fillPath(fillPath, g);
        }

        // Centered "%" text.
        painter->setPen(theme::kText);
        QFont f = option.font;
        f.setPointSizeF(f.pointSizeF() > 0 ? f.pointSizeF() : 8.5);
        painter->setFont(f);
        painter->drawText(option.rect, Qt::AlignCenter,
                          QString::number(clamped) + QStringLiteral("%"));
        painter->restore();
    }
};
} // namespace

// ---------------------------------------------------------------------------
// C2: Pure log-line HTML builder. Exported as static so tests can call it
// without a widget or Qt event loop. Called from appendLogLine with the real
// QTime; tests may pass any timestamp string.
//
// Color rules (in priority order):
//   ERR level              -> red (bold)
//   OK level / msg contains 完成|成功|ok   -> green
//   phase keywords (解码|切分|转写|fp16|provider|using) -> cyan/blue accent
//   everything else        -> default (#e0e0e0 on dark, inherits on light)
//
// The message is HTML-escaped before wrapping so paths/text with <>&" don't
// break the markup.
// ---------------------------------------------------------------------------
/*static*/ QString MainWindow::logLineHtml(const QString& level, const QString& msg,
                                            const QString& timestamp)
{
    // HTML-escape the message (protect < > & " so paths don't break markup)
    QString safe = msg.toHtmlEscaped();

    // Determine color
    QString color;
    if (level == QStringLiteral("ERR")) {
        color = QStringLiteral("#f85149");  // red
    } else if (level == QStringLiteral("OK")
               || msg.contains(QStringLiteral("完成"))
               || msg.contains(QStringLiteral("成功"))
               || msg.toLower().contains(QStringLiteral("ok"))) {
        color = QStringLiteral("#3fb950");  // green
    } else if (msg.contains(QStringLiteral("解码"))
               || msg.contains(QStringLiteral("切分"))
               || msg.contains(QStringLiteral("转写"))
               || msg.contains(QStringLiteral("fp16"))
               || msg.contains(QStringLiteral("provider"))
               || msg.contains(QStringLiteral("using"))) {
        color = QStringLiteral("#58a6ff");  // blue accent (phase/provider lines)
    } else {
        color = QStringLiteral("#c9d1d9");  // default near-white
    }

    QString boldOpen, boldClose;
    if (level == QStringLiteral("ERR")) {
        boldOpen  = QStringLiteral("<b>");
        boldClose = QStringLiteral("</b>");
    }

    // Build: [HH:MM:SS] <colored message>
    return QStringLiteral("<span style=\"color:#8b949e\">[%1]</span> "
                          "<span style=\"color:%2\">%3%4%5</span>")
        .arg(timestamp, color, boldOpen, safe, boldClose);
}

// ---------------------------------------------------------------------------
// E: pure helper — resolve the folder a row's outputs were (likely) written to.
// Mirrors EngineWorker::run's desired_dir:
//     chosenDir.isEmpty() ? parent_dir(path) : chosenDir
// We do NOT model the not-writable -> Documents/suji-转写 fallback here (rare;
// only fires on read-only source dirs); this returns the LIKELY dir for the
// "打开输出文件夹" action. Returns the path's own directory when chosenDir is
// empty, or the chosen dir otherwise.
// ---------------------------------------------------------------------------
/*static*/ QString MainWindow::rowOutputDir(const QString& path, const QString& chosenDir)
{
    if (!chosenDir.isEmpty())
        return chosenDir;
    return QFileInfo(path).absolutePath();
}

// ---------------------------------------------------------------------------
// D: programmatic app icon — a rounded-rect tile in the theme accent with a
// minimal white "S" waveform mark, so the app has an intentional icon without
// shipping an external asset (a polished asset is NEEDS-HUMAN).
// ---------------------------------------------------------------------------
/*static*/ QIcon MainWindow::makeAppIcon()
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Rounded tile background (deep panel -> base, subtle vertical gradient).
    QLinearGradient bg(0, 0, 0, 64);
    bg.setColorAt(0.0, QColor(0x22, 0x25, 0x2e));
    bg.setColorAt(1.0, QColor(0x16, 0x18, 0x1d));
    QPainterPath tile;
    tile.addRoundedRect(QRectF(2, 2, 60, 60), 14, 14);
    p.fillPath(tile, bg);
    p.setPen(QPen(theme::kBorder, 1.5));
    p.drawPath(tile);

    // A small centered "waveform": a few vertical accent bars of varying height,
    // evoking audio -> subtitle (the subject's own world).
    p.setPen(Qt::NoPen);
    p.setBrush(theme::kAccent);
    const int heights[5] = {16, 30, 40, 26, 18};
    int x = 16;
    for (int h : heights) {
        QRectF bar(x, 32 - h / 2.0, 5, h);
        QPainterPath bp; bp.addRoundedRect(bar, 2.5, 2.5);
        p.fillPath(bp, theme::kAccent);
        x += 8;
    }
    p.end();
    return QIcon(pm);
}

// ---------------------------------------------------------------------------
// D: app-wide dark QSS theme. One calm teal-blue accent (#4c9aff) over a deep,
// even background family; rounded controls; a styled gridless table with alt
// rows + accent selection; a primary (accent) Start button vs a quiet Cancel;
// slim dark scrollbars. Kept as a static so main.cpp applies it via
// qApp->setStyleSheet() and a test can assert it is non-empty.
//
// Palette (cohesive, from the mission baseline):
//   window/base   #16181d   panels/inputs #22252e   alt row #1c1f27
//   header/track  #2a2e3a   border        #333845
//   text primary  #e7e9ee   secondary     #9aa3b2
//   accent        #4c9aff   hover #63a9ff  pressed #3b86e6
//   success       #3ddc97   error  #ff6b6b
// ---------------------------------------------------------------------------
/*static*/ QString MainWindow::themeStyleSheet()
{
    return QStringLiteral(R"(
/* ── Base / window ──────────────────────────────────────────────── */
QWidget {
    background-color: #16181d;
    color: #e7e9ee;
    font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
    font-size: 9pt;
}
QMainWindow { background-color: #16181d; }

/* ── Tool bar ────────────────────────────────────────────────────── */
QToolBar {
    background-color: #16181d;
    border: none;
    border-bottom: 1px solid #333845;
    padding: 6px 8px;
    spacing: 6px;
}
QToolBar::separator {
    background: #333845;
    width: 1px;
    margin: 4px 8px;
}

/* ── Buttons (toolbar + log controls = secondary) ───────────────── */
QPushButton {
    background-color: #2a2e3a;
    color: #e7e9ee;
    border: 1px solid #333845;
    border-radius: 6px;
    padding: 6px 14px;
    min-width: 56px;
}
QPushButton:hover   { background-color: #333845; border-color: #44495a; }
QPushButton:pressed { background-color: #22252e; }
QPushButton:disabled { color: #5a6072; background-color: #1c1f27; border-color: #2a2e3a; }

/* ── Labels (secondary copy) ─────────────────────────────────────── */
QLabel { background: transparent; color: #9aa3b2; }
QLabel#emptyHint {
    color: #5a6072;
    font-size: 11pt;
    background: transparent;
}

/* ── Table ───────────────────────────────────────────────────────── */
QTableView {
    background-color: #16181d;
    alternate-background-color: #1c1f27;
    gridline-color: #22252e;
    selection-background-color: #25364f;
    selection-color: #ffffff;
    border: 1px solid #333845;
    border-radius: 8px;
    outline: none;
    padding: 2px;
}
/* E: accent drop-zone border while a drag with files hovers the window. */
QTableView[dragActive="true"] {
    border: 2px solid #4c9aff;
    background-color: #1a2330;
}
QTableView::item {
    padding: 4px 8px;
    border: none;
}
QTableView::item:selected { background-color: #25364f; color: #ffffff; }
QHeaderView::section {
    background-color: #2a2e3a;
    color: #aab2c2;
    font-weight: 600;
    padding: 7px 8px;
    border: none;
    border-right: 1px solid #333845;
    border-bottom: 1px solid #333845;
}
QHeaderView::section:last { border-right: none; }
QTableCornerButton::section { background-color: #2a2e3a; border: none; }

/* ── Splitter ────────────────────────────────────────────────────── */
QSplitter::handle { background-color: #16181d; height: 6px; }
QSplitter::handle:hover { background-color: #4c9aff; }

/* ── Log panel ───────────────────────────────────────────────────── */
QTextEdit {
    background-color: #121419;
    color: #9aa3b2;
    border: 1px solid #333845;
    border-radius: 8px;
    font-family: "Cascadia Mono", "Consolas", monospace;
    font-size: 8.5pt;
    selection-background-color: #25364f;
    padding: 4px;
}

/* ── Bottom panel background ─────────────────────────────────────── */
QWidget#bottomWidget {
    background-color: #1a1c22;
    border-top: 1px solid #333845;
    border-radius: 8px;
}

/* ── Combo box ───────────────────────────────────────────────────── */
QComboBox {
    background-color: #22252e;
    color: #e7e9ee;
    border: 1px solid #333845;
    border-radius: 6px;
    padding: 5px 10px;
    min-width: 76px;
}
QComboBox:hover  { border-color: #44495a; }
QComboBox:focus  { border-color: #4c9aff; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #9aa3b2;
    margin-right: 6px;
}
QComboBox QAbstractItemView {
    background-color: #22252e;
    color: #e7e9ee;
    selection-background-color: #25364f;
    border: 1px solid #4c9aff;
    border-radius: 6px;
    outline: none;
    padding: 2px;
}

/* ── Spin boxes ──────────────────────────────────────────────────── */
QSpinBox {
    background-color: #22252e;
    color: #e7e9ee;
    border: 1px solid #333845;
    border-radius: 6px;
    padding: 5px 8px;
    min-width: 58px;
}
QSpinBox:hover { border-color: #44495a; }
QSpinBox:focus { border-color: #4c9aff; }
QSpinBox::up-button, QSpinBox::down-button {
    background-color: #2a2e3a;
    border: none;
    width: 16px;
}
QSpinBox::up-button   { border-top-right-radius: 6px; }
QSpinBox::down-button { border-bottom-right-radius: 6px; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background-color: #4c9aff; }

/* ── Check boxes ─────────────────────────────────────────────────── */
QCheckBox { color: #e7e9ee; spacing: 6px; background: transparent; }
QCheckBox::indicator {
    width: 15px; height: 15px;
    border: 1px solid #333845;
    border-radius: 4px;
    background-color: #22252e;
}
QCheckBox::indicator:checked { background-color: #4c9aff; border-color: #4c9aff; image: none; }
QCheckBox::indicator:hover   { border-color: #4c9aff; }

/* ── Global progress bar (bottom) ────────────────────────────────── */
QProgressBar {
    background-color: #2a2e3a;
    border: 1px solid #333845;
    border-radius: 9px;
    height: 18px;
    text-align: center;
    color: #e7e9ee;
    font-size: 8pt;
}
QProgressBar::chunk {
    background-color: #4c9aff;
    border-radius: 8px;
}

/* ── Primary action button (Start / 开始) ────────────────────────── */
QPushButton#btnStart {
    background-color: #4c9aff;
    color: #ffffff;
    border: none;
    border-radius: 6px;
    padding: 7px 26px;
    font-weight: 700;
    min-width: 90px;
}
QPushButton#btnStart:hover   { background-color: #63a9ff; }
QPushButton#btnStart:pressed { background-color: #3b86e6; }
QPushButton#btnStart:disabled {
    background-color: #22252e; color: #5a6072; border: 1px solid #333845;
}

/* ── Secondary action button (Cancel / 取消) ─────────────────────── */
QPushButton#btnCancel {
    background-color: transparent;
    color: #9aa3b2;
    border: 1px solid #333845;
    border-radius: 6px;
    padding: 7px 26px;
    min-width: 90px;
}
QPushButton#btnCancel:hover {
    background-color: #2a2e3a; color: #e7e9ee; border-color: #44495a;
}
QPushButton#btnCancel:pressed  { background-color: #22252e; }
QPushButton#btnCancel:disabled { color: #44495a; border-color: #2a2e3a; }

/* ── Scroll bars (slim, dark) ────────────────────────────────────── */
QScrollBar:vertical {
    background: transparent; width: 9px; margin: 0; border-radius: 4px;
}
QScrollBar::handle:vertical {
    background: #333845; border-radius: 4px; min-height: 24px;
}
QScrollBar::handle:vertical:hover { background: #4c9aff; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal {
    background: transparent; height: 9px; border-radius: 4px;
}
QScrollBar::handle:horizontal {
    background: #333845; border-radius: 4px; min-width: 24px;
}
QScrollBar::handle:horizontal:hover { background: #4c9aff; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

/* ── Tool tips ───────────────────────────────────────────────────── */
QToolTip {
    background-color: #22252e;
    color: #e7e9ee;
    border: 1px solid #4c9aff;
    border-radius: 6px;
    padding: 4px 8px;
}

/* ── Context menu ────────────────────────────────────────────────── */
QMenu {
    background-color: #22252e;
    color: #e7e9ee;
    border: 1px solid #333845;
    border-radius: 8px;
    padding: 4px;
}
QMenu::item { padding: 6px 22px; border-radius: 5px; }
QMenu::item:selected { background-color: #25364f; color: #ffffff; }
QMenu::separator { height: 1px; background: #333845; margin: 4px 8px; }
)");
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("suji 批量转写"));
    setWindowIcon(makeAppIcon());   // D: intentional programmatic icon (no external asset)
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

    // E: tooltips (短说明 + shortcut hint where relevant)
    btnAddFiles->setToolTip(tr("选择一个或多个媒体文件加入列表 (Ctrl+O)"));
    btnAddFolder->setToolTip(tr("递归添加文件夹内的所有媒体文件"));
    btnClear->setToolTip(tr("清空文件列表"));
    btnOutDir->setToolTip(tr("选择输出目录（留空则写到每个源文件旁）"));

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
    m_model->setHorizontalHeaderLabels({tr("文件"), tr("状态"), tr("进度"), tr("段数"), tr("错误")});

    m_table = new QTableView(this);
    m_table->setModel(m_model);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    // Per-file progress bar delegate on the 进度 column ("每个视频分开").
    m_table->setItemDelegateForColumn(ColProgress, new ProgressBarDelegate(m_table));
    m_table->horizontalHeader()->setSectionResizeMode(ColFile,     QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColStatus,   QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColProgress, QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColSegs,     QHeaderView::Fixed);
    m_table->horizontalHeader()->setSectionResizeMode(ColErr,      QHeaderView::Fixed);
    m_table->setColumnWidth(ColStatus,   80);
    m_table->setColumnWidth(ColProgress, 140);
    m_table->setColumnWidth(ColSegs,     60);
    m_table->setColumnWidth(ColErr,      200);
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(30);  // taller rows for readability

    // E: row context menu (移除 / 打开输出文件夹 / 打开转写结果) + double-click → open output.
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested,
            this,    &MainWindow::onTableContextMenu);
    connect(m_table, &QTableView::doubleClicked,
            this,    &MainWindow::onTableDoubleClicked);

    // D: empty-state overlay — a centered muted hint over the table when 0 rows.
    // Parented to the viewport so it floats above the (empty) cells; repositioned
    // in resizeEvent and shown/hidden by updateEmptyState().
    m_emptyHint = new QLabel(tr("拖入视频/音频，或点「添加文件」开始"), m_table->viewport());
    m_emptyHint->setObjectName(QStringLiteral("emptyHint"));
    m_emptyHint->setAlignment(Qt::AlignCenter);
    m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);  // clicks fall through to the table
    m_emptyHint->setWordWrap(false);  // short one-line hint; wrapping fragmented it when mis-sized
    m_emptyHint->adjustSize();
    // The window resizeEvent fires before the table viewport has its final geometry,
    // which left the hint stuck top-left. Re-center on the VIEWPORT's own resize.
    m_table->viewport()->installEventFilter(this);

    // -----------------------------------------------------------------------
    // Log panel — splitter below the file table
    // Log uses QTextEdit (rich text) for timestamp + color-coded lines (C2).
    // -----------------------------------------------------------------------
    auto* splitter = new QSplitter(Qt::Vertical, central);

    // Log area: QTextEdit (rich/HTML) + small button bar on the right
    auto* logAreaWidget = new QWidget(splitter);
    auto* logAreaLayout = new QHBoxLayout(logAreaWidget);
    logAreaLayout->setContentsMargins(0, 0, 0, 0);
    logAreaLayout->setSpacing(4);

    m_log = new QTextEdit(logAreaWidget);
    m_log->setReadOnly(true);
    m_log->document()->setMaximumBlockCount(5000);
    QFont mono(QStringLiteral("Consolas"), 9);
    mono.setStyleHint(QFont::Monospace);
    m_log->setFont(mono);
    m_log->setAcceptRichText(true);

    // Log control buttons (clear + copy)
    auto* logBtnCol = new QWidget(logAreaWidget);
    auto* logBtnLayout = new QVBoxLayout(logBtnCol);
    logBtnLayout->setContentsMargins(0, 0, 0, 0);
    logBtnLayout->setSpacing(4);
    auto* btnLogClear = new QPushButton(tr("清空"), logBtnCol);
    auto* btnLogCopy  = new QPushButton(tr("复制"), logBtnCol);
    btnLogClear->setMaximumWidth(52);
    btnLogCopy->setMaximumWidth(52);
    logBtnLayout->addWidget(btnLogClear);
    logBtnLayout->addWidget(btnLogCopy);
    logBtnLayout->addStretch();

    logAreaLayout->addWidget(m_log, /*stretch=*/1);
    logAreaLayout->addWidget(logBtnCol, /*stretch=*/0);

    connect(btnLogClear, &QPushButton::clicked, m_log, &QTextEdit::clear);
    connect(btnLogCopy, &QPushButton::clicked, this, [this](){
        QApplication::clipboard()->setText(m_log->toPlainText());
    });

    splitter->addWidget(m_table);
    splitter->addWidget(logAreaWidget);
    splitter->setSizes({400, 180});

    rootLayout->addWidget(splitter, /*stretch=*/1);

    // -----------------------------------------------------------------------
    // Bottom panel
    // -----------------------------------------------------------------------
    auto* bottomWidget = new QWidget(this);
    bottomWidget->setObjectName(QStringLiteral("bottomWidget"));
    auto* bottomLayout = new QVBoxLayout(bottomWidget);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(4);

    // Settings row: provider + format checkboxes
    auto* settingsRow = new QHBoxLayout;
    settingsRow->setSpacing(12);

    // 转写模式 (transcription-quality preset): picks the model + recommended backend.
    // Default = 准确度(Qwen3). The 推理后端 combo below is an ADVANCED override.
    auto* modeLabel = new QLabel(tr("转写模式:"), bottomWidget);
    m_mode = new QComboBox(bottomWidget);
    m_mode->addItem(tr("准确度(Qwen3)"));   // index 0 = Mode::Qwen3 (default)
    m_mode->addItem(tr("速度(AED)"));        // index 1 = Mode::Aed
    m_mode->addItem(tr("词级字幕(CTC)"));    // index 2 = Mode::Ctc
    m_mode->setItemData(0, tr("Qwen3 模型,最准确(人名 + 中英混),默认在 CPU 上运行。"), Qt::ToolTipRole);
    m_mode->setItemData(1, tr("fp16 AED 模型,最快但精度略低,需要 CUDA GPU。"), Qt::ToolTipRole);
    m_mode->setItemData(2, tr("int8 CTC 模型,带逐字时间戳,适合精细字幕对齐。"), Qt::ToolTipRole);
    m_mode->setToolTip(tr("转写模式:准确度(Qwen3) / 速度(AED) / 词级字幕(CTC)。"
                          "模式决定使用的模型与推荐后端;下方「推理后端」为高级覆盖。"));
    m_mode->setCurrentIndex(0);

    auto* providerLabel = new QLabel(tr("推理后端:"), bottomWidget);
    m_provider = new QComboBox(bottomWidget);
    m_provider->addItem(QStringLiteral("auto"));
    m_provider->addItem(QStringLiteral("cpu"));
    m_provider->addItem(QStringLiteral("cuda"));
    m_provider->addItem(QStringLiteral("hetero"));
    m_provider->setToolTip(tr("推理后端(高级):auto 跟随所选转写模式的推荐后端;"
                              "选 cpu/cuda/hetero 则覆盖模式默认。"));

    m_chkSrt  = new QCheckBox(QStringLiteral("SRT"),  bottomWidget);
    m_chkVtt  = new QCheckBox(QStringLiteral("VTT"),  bottomWidget);
    m_chkJson = new QCheckBox(QStringLiteral("JSON"), bottomWidget);
    m_chkMd   = new QCheckBox(QStringLiteral("MD"),   bottomWidget);
    m_chkSrt->setChecked(true);
    m_chkVtt->setChecked(true);
    m_chkJson->setChecked(true);
    m_chkMd->setChecked(true);

    // G12: batch-size and in-flight override spinboxes (0 = auto)
    m_spnBatch = new QSpinBox(bottomWidget);
    m_spnBatch->setRange(0, 64);
    m_spnBatch->setSpecialValueText(tr("自动"));
    m_spnBatch->setValue(0);
    m_spnBatch->setToolTip(tr("批大小 (0=自动)"));

    m_spnInFlight = new QSpinBox(bottomWidget);
    m_spnInFlight->setRange(0, 32);
    m_spnInFlight->setSpecialValueText(tr("自动"));
    m_spnInFlight->setValue(0);
    m_spnInFlight->setToolTip(tr("并行文件 (0=自动)"));

    settingsRow->addWidget(modeLabel);
    settingsRow->addWidget(m_mode);
    settingsRow->addSpacing(16);
    settingsRow->addWidget(providerLabel);
    settingsRow->addWidget(m_provider);
    settingsRow->addSpacing(16);
    settingsRow->addWidget(new QLabel(tr("输出格式:"), bottomWidget));
    settingsRow->addWidget(m_chkSrt);
    settingsRow->addWidget(m_chkVtt);
    settingsRow->addWidget(m_chkJson);
    settingsRow->addWidget(m_chkMd);
    settingsRow->addSpacing(16);
    settingsRow->addWidget(new QLabel(tr("批大小:"), bottomWidget));
    settingsRow->addWidget(m_spnBatch);
    settingsRow->addWidget(new QLabel(tr("并行文件:"), bottomWidget));
    settingsRow->addWidget(m_spnInFlight);
    settingsRow->addStretch();

    // Progress row
    auto* progressRow = new QHBoxLayout;
    m_progress = new QProgressBar(bottomWidget);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(true);
    m_progress->setFormat(QStringLiteral("%p%"));
    m_statusLabel = new QLabel(tr("就绪"), bottomWidget);
    m_statusLabel->setMinimumWidth(260);
    progressRow->addWidget(m_progress, /*stretch=*/1);
    progressRow->addWidget(m_statusLabel);

    // Action row: [完成后打开输出] ... Start / Cancel
    auto* actionRow = new QHBoxLayout;
    // E (optional): open the first output folder when the batch finishes.
    m_chkOpenOnFinish = new QCheckBox(tr("完成后打开输出文件夹"), bottomWidget);
    m_chkOpenOnFinish->setToolTip(tr("批量完成后自动在文件管理器中打开输出目录"));
    actionRow->addWidget(m_chkOpenOnFinish);
    actionRow->addStretch();
    m_btnStart  = new QPushButton(tr("开始"), bottomWidget);
    m_btnCancel = new QPushButton(tr("取消"), bottomWidget);
    m_btnStart->setObjectName(QStringLiteral("btnStart"));
    m_btnCancel->setObjectName(QStringLiteral("btnCancel"));
    m_btnCancel->setEnabled(false);
    m_btnStart->setMinimumWidth(80);
    m_btnCancel->setMinimumWidth(80);
    m_btnStart->setToolTip(tr("开始转写列表中的所有文件 (Ctrl+Enter)"));
    m_btnCancel->setToolTip(tr("取消正在进行的转写 (Esc)"));
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

    connect(worker_, &EngineWorker::started,
            this,    &MainWindow::onWorkerStarted);
    connect(worker_, &EngineWorker::progress,
            this,    &MainWindow::onWorkerProgress);
    connect(worker_, &EngineWorker::fileProgress,
            this,    &MainWindow::onFileProgress);
    connect(worker_, &EngineWorker::fileResult,
            this,    &MainWindow::onWorkerFileResult);
    connect(worker_, &EngineWorker::finished,
            this,    &MainWindow::onWorkerFinished);

    // Clean up worker object when thread finishes
    connect(workerThread_, &QThread::finished,
            worker_,       &QObject::deleteLater);

    workerThread_->start();

    // G14: per-second status repaint timer (created stopped; started in onWorkerStarted).
    m_tick = new QTimer(this);
    m_tick->setInterval(1000);
    connect(m_tick, &QTimer::timeout, this, &MainWindow::onSecondTick);

    // Route core log_info/log_err -> GUI log panel (thread-safe via queued connection)
    QPointer<MainWindow> self(this);
    set_log_sink([self](const std::string& lvl, const std::string& m) {
        if (!self) return;
        QMetaObject::invokeMethod(self, "appendLogLine", Qt::QueuedConnection,
            Q_ARG(QString, QString::fromStdString(lvl)),
            Q_ARG(QString, QString::fromUtf8(m.c_str())));
    });

    // G11: restore persisted settings AFTER all widgets are constructed
    loadSettings();

    // E: keyboard shortcuts. WindowShortcut context so they don't hijack typing in
    // the log/spinboxes — Qt routes a shortcut to the focused widget's own handlers
    // first, and the action callbacks themselves no-op when not applicable.
    auto* scAdd = new QShortcut(QKeySequence(QKeySequence::Open), this);  // Ctrl+O
    connect(scAdd, &QShortcut::activated, this, &MainWindow::onAddFiles);

    auto* scCancel = new QShortcut(QKeySequence(Qt::Key_Escape), this);   // Esc
    connect(scCancel, &QShortcut::activated, this, [this]() {
        if (m_btnCancel && m_btnCancel->isEnabled()) onCancel();
    });

    // Ctrl+Enter / Ctrl+Return → 开始 (only when idle). A bare Space/Enter would
    // hijack typing/spinbox editing, so we require the Ctrl modifier.
    auto* scStart1 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    auto* scStart2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter),  this);
    auto startIfIdle = [this]() { if (m_btnStart && m_btnStart->isEnabled()) onStart(); };
    connect(scStart1, &QShortcut::activated, this, startIfIdle);
    connect(scStart2, &QShortcut::activated, this, startIfIdle);

    // D/E: initial empty-state + Start-enabled reflect the (possibly empty) list.
    updateEmptyState();
    m_btnStart->setEnabled(m_model->rowCount() > 0);
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
MainWindow::~MainWindow()
{
    set_log_sink({});   // clear before worker teardown to avoid dangling callback
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
    updateEmptyState();
    if (m_btnStart) m_btnStart->setEnabled(false);  // empty list -> nothing to start
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

    // Output directory: a chosen dir, or empty => write next to each source file
    // (the worker resolves an empty outDir to the input's own directory, matching the
    // "（与源文件相同）" label, which we leave untouched when no dir is chosen).
    const QString outDir = m_outputDir;
    if (!outDir.isEmpty())
        QDir().mkpath(outDir);

    // E: capture the chosen output dir for THIS run so row context-menu / double-click
    // resolves the same dir the worker used, even if the field is edited later.
    m_runOutputDir   = outDir;
    m_firstOutputDir.clear();   // reset; set from the first completed file (E open-on-finish)

    // Reset UI state
    m_btnStart->setEnabled(false);
    m_btnCancel->setEnabled(true);
    m_progress->setValue(0);

    // Reset all row statuses to "待处理"
    for (int r = 0; r < m_model->rowCount(); ++r) {
        setRowStatus(r, tr("待处理"));
        if (auto* item = m_model->item(r, ColSegs)) item->setText(QString());
        setRowProgress(r, -1);   // clear per-file progress bar (not started)
        setRowError(r, QString());
    }
    setStatusText(tr("正在初始化…"));

    m_startTime = std::chrono::steady_clock::now();
    m_transStartAudio = -1.0;   // reset transcription-phase anchor for this run

    // G14: reset live-feedback snapshot + EMA state for this run.
    m_jobRunning      = true;
    m_lastFilesDone   = 0;
    m_lastFilesTotal  = files.size();
    m_lastAudioSec    = 0.0;
    m_lastTotalAudio  = 0.0;
    m_lastCpuSegs     = 0;
    m_lastGpuSegs     = 0;
    m_lastSegsDone    = 0;
    m_lastSegsTotal   = 0;
    m_lastPct         = 0;
    m_emaRate         = 0.0;
    m_prevAudioSec    = -1.0;

    // G11: persist settings when run starts
    saveSettings();

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
        Q_ARG(bool,        wantMd()),
        Q_ARG(int,         batchOverride()),
        Q_ARG(int,         inFlightOverride()),
        Q_ARG(int,         mode())
    );
}

void MainWindow::onCancel()
{
    // G14: stop the per-second tick so it can't overwrite the "正在取消…" message
    // while the worker winds down; onWorkerFinished writes the final summary.
    if (m_tick) m_tick->stop();
    setStatusText(tr("正在取消…"));
    if (worker_) worker_->requestCancel();   // direct atomic store; thread-safe, takes effect immediately
}

// ---------------------------------------------------------------------------
// Worker signal handlers
// ---------------------------------------------------------------------------
void MainWindow::onWorkerStarted(QString provider, int filesTotal)
{
    m_lastFilesTotal = filesTotal;
    // During init+decode+VAD there is no transcription % yet — say so explicitly
    // instead of "转写" so the wording matches what's actually happening. The first
    // onWorkerProgress (real transcription output) replaces this with the live line.
    setStatusText(tr("正在用 %1 解码 / 准备转写 %2 个文件…")
        .arg(provider.toUpper())
        .arg(filesTotal));

    // Flip every pending row to "in-progress"
    const int n = m_model->rowCount();
    for (int r = 0; r < n; ++r) {
        if (m_model->item(r, ColStatus)->text() == tr("待处理"))
            setRowStatus(r, tr("处理中"));
    }
    // Busy/indeterminate during init+decode (no measurable % yet) so the bar
    // animates instead of sitting at a dead 0%. onWorkerProgress switches it
    // to a real percentage on the first transcription output.
    m_progress->setRange(0, 0);

    // G14: start the per-second status repaint so elapsed/ETA never appear frozen,
    // even during the silent decode/VAD window before the first engine callback.
    m_jobRunning = true;
    if (m_tick) m_tick->start();
}

void MainWindow::onWorkerProgress(int filesDone, int filesTotal, double audioSec, double totalAudioSec,
                                  long long cpuSegs, long long gpuSegs,
                                  long long segsDone, long long segsTotal)
{
    auto now = std::chrono::steady_clock::now();
    // First progress callback = transcription has begun; anchor here so the elapsed/
    // ETA reflect the real transcription rate, not the init/decode/VAD startup.
    if (m_transStartAudio < 0.0) { m_transStartTime = now; m_transStartAudio = audioSec; }

    // ------------------------------------------------------------------
    // G14: EMA 倍速. The previous anchored "(audioSec-anchor)/elapsed" window is tiny
    // and noisy early (reads a misleading ~1.8x when the true rate is ~6x). Instead
    // track the PREVIOUS callback's (audioSec, time), take the instantaneous rate
    // Δaudio/Δtime, and smooth it with an EMA so the figure settles on the true
    // rate within a few seconds.
    if (m_prevAudioSec >= 0.0) {
        double dt = std::chrono::duration<double>(now - m_prevRateTime).count();
        double da = audioSec - m_prevAudioSec;
        if (dt > 1e-3 && da >= 0.0) {
            double inst = da / dt;                       // instantaneous transcription speed
            constexpr double kAlpha = 0.3;               // weight on the newest sample
            m_emaRate = (m_emaRate <= 0.0) ? inst
                                           : kAlpha * inst + (1.0 - kAlpha) * m_emaRate;
        }
    }
    m_prevAudioSec = audioSec;
    m_prevRateTime = now;

    // Segment-based progress: a concrete, determinate bar that reaches 100% when every
    // segment is routed. segsTotal grows as files finish VAD, so early on (segsTotal==0,
    // still decoding/VAD) we keep the marquee/indeterminate bar. Once segments exist we
    // switch to a real percentage. This replaces the old speech-seconds/full-duration
    // ratio that capped below 100% on files with silence and froze during long batches.
    int pct;
    if (segsTotal > 0) {
        pct = std::min(100, static_cast<int>(100 * segsDone / segsTotal));
        // Leave busy/indeterminate mode on the first real progress, then show %.
        if (m_progress->maximum() == 0)
            m_progress->setRange(0, 100);
        m_progress->setValue(pct);
    } else {
        // Still decoding/VAD — no segments queued yet; keep the indeterminate marquee.
        pct = 0;
    }

    // G14: store the latest snapshot so the 1s tick can recompute elapsed/ETA between
    // callbacks without waiting for the (possibly seconds-away) next engine progress.
    m_lastFilesDone  = filesDone;
    m_lastFilesTotal = filesTotal;
    m_lastAudioSec   = audioSec;
    m_lastTotalAudio = totalAudioSec;
    m_lastCpuSegs    = cpuSegs;
    m_lastGpuSegs    = gpuSegs;
    m_lastSegsDone   = segsDone;
    m_lastSegsTotal  = segsTotal;
    m_lastPct        = pct;

    renderStatusLine();
}

// ---------------------------------------------------------------------------
// B: pure helper — derive the per-file phase string from segment counters.
// segsTotal==0: file is still in decode/VAD ("解码中").
// 0 < segsDone < segsTotal: transcription underway ("转写中").
// segsDone==segsTotal (and >0): all segments routed (final 完成/失败/取消 comes
//   from onWorkerFileResult, so we don't set 完成 here).
// Returns an empty string when the phase is terminal and should not be overwritten.
// ---------------------------------------------------------------------------
static QString filePhaseStr(int segsDone, int segsTotal)
{
    if (segsTotal == 0)
        return QStringLiteral("解码中");
    if (segsDone < segsTotal)
        return QStringLiteral("转写中");
    // segsDone == segsTotal: transcription routed; let fileResult set the terminal state.
    return QString();
}

// ---------------------------------------------------------------------------
// G14: render the bottom status line from the current snapshot. Called by both
// onWorkerProgress (fresh numbers) and onSecondTick (recomputes elapsed/ETA from
// the held snapshot) so the line keeps moving even between sparse callbacks.
// ---------------------------------------------------------------------------
void MainWindow::renderStatusLine()
{
    auto now = std::chrono::steady_clock::now();
    double overall = std::chrono::duration<double>(now - m_startTime).count();

    // 倍速: prefer the smoothed EMA; before the second callback seeds it, fall back
    // to the overall average so we never show a wild first value or a dead 0.
    double speed = (m_emaRate > 0.0)
        ? m_emaRate
        : ((overall > 0.0) ? m_lastAudioSec / overall : 0.0);

    // Live ETA from % complete, recomputed against the up-to-the-second elapsed time.
    QString etaStr;
    if (m_lastPct > 0) {
        double etaSec = overall * (100.0 - m_lastPct) / m_lastPct;
        int etaMin  = static_cast<int>(etaSec) / 60;
        int etaSecs = static_cast<int>(etaSec) % 60;
        etaStr = tr("  剩余约 %1:%2")
            .arg(etaMin, 2, 10, QChar('0'))
            .arg(etaSecs, 2, 10, QChar('0'));
    }

    // Live CPU/GPU split (hetero only; both 0 => single-provider, omit). Build the
    // literal '%' via QStringLiteral so QString::arg never mis-reads it as a marker.
    QString splitStr;
    const long long segTot = m_lastCpuSegs + m_lastGpuSegs;
    if (segTot > 0) {
        int cpuPct = static_cast<int>(m_lastCpuSegs * 100 / segTot);
        splitStr = QStringLiteral("  CPU ") + QString::number(cpuPct) + QStringLiteral("% / GPU ")
                 + QString::number(100 - cpuPct) + QStringLiteral("%");
    }

    int elapsedMin  = static_cast<int>(overall) / 60;
    int elapsedSecs = static_cast<int>(overall) % 60;

    // Concrete segment count the user can watch tick up every batch (已转写 N/M 段).
    // Build via QStringLiteral so the segment numbers can't be mis-read as arg markers.
    const QString segStr = QStringLiteral("  已转写 ")
        + QString::number(m_lastSegsDone) + QStringLiteral("/")
        + QString::number(m_lastSegsTotal) + QStringLiteral(" 段");

    const QString pctStr = QString::number(m_lastPct) + QStringLiteral("%");
    setStatusText(tr("处理中 %1  %2/%3 文件%4  %5 倍速  (已转写 %6 秒, 用时 %7:%8)%9%10")
        .arg(pctStr)
        .arg(m_lastFilesDone)
        .arg(m_lastFilesTotal)
        .arg(segStr)
        .arg(speed, 0, 'f', 1)
        .arg(static_cast<int>(m_lastAudioSec))
        .arg(elapsedMin, 2, 10, QChar('0'))
        .arg(elapsedSecs, 2, 10, QChar('0'))
        .arg(splitStr)
        .arg(etaStr));
}

// ---------------------------------------------------------------------------
// G14: 1s repaint. While a job runs, refresh the status line from the held
// snapshot so elapsed/ETA/throughput keep ticking even when the engine hasn't
// produced a fresh progress callback (e.g. a slow GPU batch). Before the first
// real progress arrives, leave the "正在解码 / 准备转写…" wording untouched.
// ---------------------------------------------------------------------------
void MainWindow::onSecondTick()
{
    if (!m_jobRunning) return;
    // Still in decode/VAD with nothing queued yet -> keep the "preparing" status as-is.
    // Once any segment exists (segs_total>0) or audio has been transcribed, render the
    // live line so the concrete 已转写 N/M 段 count keeps ticking between callbacks.
    if (m_lastSegsTotal <= 0 && m_lastTotalAudio <= 0.0 && m_lastAudioSec <= 0.0)
        return;
    renderStatusLine();
}

// ---------------------------------------------------------------------------
// PER-FILE progress ("每个视频分开"): the worker emits one fileProgress per file
// per engine callback, keyed by the original input PATH. Find the matching row
// (same path lookup as onWorkerFileResult), update its 进度 bar + 段数 cell, and
// drive the status column through per-file phases:
//   待处理 -> 解码中 (segsTotal==0, decode/VAD still running)
//   解码中 -> 转写中 (0 < segsDone < segsTotal, transcription underway)
// Terminal states (完成/失败/取消) are set only by onWorkerFileResult — never
// overwritten here.
// ---------------------------------------------------------------------------
void MainWindow::onFileProgress(QString path, int percent, int segsDone, int segsTotal)
{
    const int n = m_model->rowCount();
    for (int r = 0; r < n; ++r) {
        if (m_model->item(r, ColFile)->data(Qt::UserRole).toString() != path)
            continue;
        setRowProgress(r, percent);
        // Show this file's own segment count (done/total) once it has segments.
        if (segsTotal > 0) {
            if (auto* item = m_model->item(r, ColSegs))
                item->setText(QString::number(segsDone) + QStringLiteral("/")
                            + QString::number(segsTotal));
        }
        // B: drive per-file phase. Only advance non-terminal status cells.
        if (auto* st = m_model->item(r, ColStatus)) {
            const QString cur = st->text();
            // Never overwrite terminal states set by fileResult.
            const bool isTerminal = (cur == tr("完成") || cur == tr("失败") || cur == tr("取消"));
            if (!isTerminal) {
                const QString phase = filePhaseStr(segsDone, segsTotal);
                if (!phase.isEmpty())
                    setRowStatus(r, phase);   // phase is already a final display string
            }
        }
        return;
    }
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
                setRowProgress(r, 100);   // completed file: fill its per-file bar
                appendLogLine(QStringLiteral("OK"),
                    tr("完成: %1 (%2 段)").arg(QFileInfo(path).fileName()).arg(segments));
                // E: remember the first completed file's output dir for 完成后打开输出.
                if (m_firstOutputDir.isEmpty())
                    m_firstOutputDir = rowOutputDir(path, m_runOutputDir);
            } else if (err == QStringLiteral("cancelled")) {
                setRowStatus(r, tr("取消"));
                appendLogLine(QStringLiteral("INFO"),
                    tr("取消: %1").arg(QFileInfo(path).fileName()));
            } else {
                setRowStatus(r, tr("失败"));
                setRowError(r, err);
                appendLogLine(QStringLiteral("ERR"),
                    tr("失败: %1 — %2").arg(QFileInfo(path).fileName(), err));
            }
            return;
        }
    }
}

void MainWindow::onWorkerFinished(int ok, int failed, int cancelled, double wallSec)
{
    // G14: stop the per-second tick so the final summary line isn't overwritten.
    m_jobRunning = false;
    if (m_tick) m_tick->stop();

    // Restore determinate mode before setting value
    m_progress->setRange(0, 100);
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

    // E (optional): open the first output folder when the batch finishes, if the
    // user opted in and at least one file produced output this run.
    if (m_chkOpenOnFinish && m_chkOpenOnFinish->isChecked() && !m_firstOutputDir.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_firstOutputDir));
    }
}

// ---------------------------------------------------------------------------
// Drag-and-drop (E: accent drop-zone highlight while a file drag hovers)
// ---------------------------------------------------------------------------
void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasUrls()) {
        setDragHighlight(true);   // accent border on the table while dragging files in
        event->acceptProposedAction();
    } else {
        QMainWindow::dragEnterEvent(event);
    }
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent* event)
{
    setDragHighlight(false);   // clear the highlight when the drag leaves the window
    QMainWindow::dragLeaveEvent(event);
}

void MainWindow::dropEvent(QDropEvent* event)
{
    setDragHighlight(false);   // clear the highlight; the files are now being added
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
    // G11: persist settings before teardown
    saveSettings();

    // Direct cancel: the atomic store reaches the blocked worker thread immediately.
    // Unbounded wait() is safe because the direct cancel makes run() return within seconds.
    if (worker_) worker_->requestCancel();
    workerThread_->quit();
    workerThread_->wait(); // unbounded — direct cancel ensures run() returns promptly
    QMainWindow::closeEvent(event);
}

// ---------------------------------------------------------------------------
// D: keep the empty-state hint centered over the table viewport on resize.
// ---------------------------------------------------------------------------
void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    updateEmptyState();   // re-centers m_emptyHint over the current viewport
}

// The table viewport gets its real geometry AFTER the window resizeEvent fires, so
// centering there used a stale rect (hint stuck top-left). Re-center on the
// viewport's own resize for correct timing.
bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    if (m_table && obj == m_table->viewport() && event->type() == QEvent::Resize)
        updateEmptyState();
    return QMainWindow::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------
// E: row context menu — 移除 / 打开输出文件夹 / 打开转写结果.
// The last two are enabled only for a 完成 row (its outputs exist on disk).
// ---------------------------------------------------------------------------
void MainWindow::onTableContextMenu(const QPoint& pos)
{
    const QModelIndex idx = m_table->indexAt(pos);
    if (!idx.isValid())
        return;
    const int row = idx.row();

    const QString status = m_model->item(row, ColStatus)
                         ? m_model->item(row, ColStatus)->text() : QString();
    const bool done = (status == tr("完成"));

    QMenu menu(this);
    QAction* actRemove   = menu.addAction(tr("移除"));
    menu.addSeparator();
    QAction* actOpenDir  = menu.addAction(tr("打开输出文件夹"));
    QAction* actOpenFile = menu.addAction(tr("打开转写结果"));
    // Output-related actions only make sense once the file is done.
    actOpenDir->setEnabled(done);
    actOpenFile->setEnabled(done);

    QAction* chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == actRemove) {
        m_model->removeRow(row);
        updateEmptyState();
        if (m_model->rowCount() == 0 && m_btnStart && m_btnCancel
            && !m_btnCancel->isEnabled())
            m_btnStart->setEnabled(false);
    } else if (chosen == actOpenDir) {
        openRowOutputFolder(row);
    } else if (chosen == actOpenFile) {
        openRowTranscript(row);
    }
}

// ---------------------------------------------------------------------------
// E: double-click a 完成 row -> open its output folder.
// ---------------------------------------------------------------------------
void MainWindow::onTableDoubleClicked(const QModelIndex& index)
{
    if (!index.isValid())
        return;
    const int row = index.row();
    auto* st = m_model->item(row, ColStatus);
    if (st && st->text() == tr("完成"))
        openRowOutputFolder(row);
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
    itemStatus->setForeground(QBrush(QColor(0x70, 0x73, 0x7d)));  // muted gray
    auto* itemProg   = new QStandardItem(QString());
    itemProg->setData(-1, ProgressRole);   // -1 = not started (delegate paints empty track)
    auto* itemSegs   = new QStandardItem(QString());
    auto* itemErr    = new QStandardItem(QString());

    m_model->appendRow({itemFile, itemStatus, itemProg, itemSegs, itemErr});

    // D/E: a non-empty list hides the hint and (when idle) enables 开始.
    updateEmptyState();
    if (m_btnStart && m_btnStart->isEnabled() == false && m_btnCancel
        && !m_btnCancel->isEnabled())
        m_btnStart->setEnabled(true);
}

void MainWindow::addFolder(const QString& dir)
{
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (isMediaFile(path))
            addInputFile(path);
    }
}

// ---------------------------------------------------------------------------
// E: toggle the accent drop-zone border on the table. We set a dynamic property
// the QSS targets (QTableView[dragActive="true"]) and re-polish so the style
// re-evaluates immediately.
// ---------------------------------------------------------------------------
void MainWindow::setDragHighlight(bool on)
{
    if (!m_table) return;
    if (m_table->property("dragActive").toBool() == on)
        return;
    m_table->setProperty("dragActive", on);
    m_table->style()->unpolish(m_table);
    m_table->style()->polish(m_table);
    m_table->update();
}

// ---------------------------------------------------------------------------
// D: show/hide + center the empty-state hint over the table viewport. Shown only
// when the model has 0 rows; re-centered on every call (resize + add/remove).
// ---------------------------------------------------------------------------
void MainWindow::updateEmptyState()
{
    if (!m_emptyHint || !m_table) return;
    const bool empty = (m_model->rowCount() == 0);
    m_emptyHint->setVisible(empty);
    if (!empty) return;
    // Center over the viewport (the hint is parented to it).
    const QRect vp = m_table->viewport()->rect();
    const QSize hint = m_emptyHint->sizeHint();
    const int w = std::min(hint.width()  + 24, vp.width());
    const int h = hint.height() + 8;
    m_emptyHint->setGeometry((vp.width()  - w) / 2,
                             (vp.height() - h) / 2, w, h);
}

// ---------------------------------------------------------------------------
// E: resolve a row's effective output dir. We don't get the worker's effective
// dir back over the wire, so recompute the LIKELY dir from the stored path +
// the run's chosen dir (rowOutputDir mirrors the worker's desired_dir logic).
// ---------------------------------------------------------------------------
QString MainWindow::rowEffectiveOutputDir(int row) const
{
    auto* itemFile = m_model->item(row, ColFile);
    if (!itemFile) return QString();
    const QString path = itemFile->data(Qt::UserRole).toString();
    return rowOutputDir(path, m_runOutputDir);
}

// ---------------------------------------------------------------------------
// E: open a row's output folder in the system file explorer.
// ---------------------------------------------------------------------------
void MainWindow::openRowOutputFolder(int row)
{
    const QString dir = rowEffectiveOutputDir(row);
    if (dir.isEmpty() || !QDir(dir).exists()) {
        appendLogLine(QStringLiteral("INFO"), tr("输出目录不存在: %1").arg(dir));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// ---------------------------------------------------------------------------
// E: open a row's produced transcript. Prefers .srt, then .md, then .vtt/.json,
// using the same stem the worker writes (the source file's basename without
// extension). Opens it with the OS default app via QDesktopServices.
// ---------------------------------------------------------------------------
void MainWindow::openRowTranscript(int row)
{
    auto* itemFile = m_model->item(row, ColFile);
    if (!itemFile) return;
    const QString path = itemFile->data(Qt::UserRole).toString();
    const QString dir  = rowEffectiveOutputDir(row);
    const QString stem = QFileInfo(path).completeBaseName();

    static const char* kExts[] = { "srt", "md", "vtt", "json" };
    for (const char* ext : kExts) {
        const QString cand = dir + QStringLiteral("/") + stem
                           + QStringLiteral(".") + QString::fromLatin1(ext);
        if (QFileInfo::exists(cand)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(cand));
            return;
        }
    }
    appendLogLine(QStringLiteral("INFO"),
        tr("未找到转写结果文件: %1").arg(dir + QStringLiteral("/") + stem));
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
int     MainWindow::mode() const       { return m_mode ? m_mode->currentIndex() : 0; }
bool    MainWindow::wantSrt()  const   { return m_chkSrt->isChecked(); }
bool    MainWindow::wantVtt()  const   { return m_chkVtt->isChecked(); }
bool    MainWindow::wantJson() const   { return m_chkJson->isChecked(); }
bool    MainWindow::wantMd()   const   { return m_chkMd->isChecked(); }
int     MainWindow::batchOverride()    const { return m_spnBatch    ? m_spnBatch->value()    : 0; }
int     MainWindow::inFlightOverride() const { return m_spnInFlight ? m_spnInFlight->value() : 0; }

// ---------------------------------------------------------------------------
// G11: QSettings persistence
// ---------------------------------------------------------------------------
void MainWindow::loadSettings()
{
    QSettings s;
    // provider combo
    const QString prov = s.value(QStringLiteral("gui/provider"), QStringLiteral("auto")).toString();
    int idx = m_provider->findText(prov);
    if (idx >= 0) m_provider->setCurrentIndex(idx);

    // 转写模式 (default 0 = 准确度(Qwen3)); clamp to the valid index range.
    if (m_mode) {
        int mIdx = s.value(QStringLiteral("gui/mode"), 0).toInt();
        if (mIdx < 0 || mIdx >= m_mode->count()) mIdx = 0;
        m_mode->setCurrentIndex(mIdx);
    }

    // format checkboxes (default all on)
    m_chkSrt ->setChecked(s.value(QStringLiteral("gui/srt"),  true).toBool());
    m_chkVtt ->setChecked(s.value(QStringLiteral("gui/vtt"),  true).toBool());
    m_chkJson->setChecked(s.value(QStringLiteral("gui/json"), true).toBool());
    m_chkMd  ->setChecked(s.value(QStringLiteral("gui/md"),   true).toBool());

    // output dir
    const QString odir = s.value(QStringLiteral("gui/outputDir"), QString()).toString();
    if (!odir.isEmpty()) {
        m_outputDir = odir;
        m_outDirLabel->setText(odir);
    }

    // G12 spinboxes (default 0 = auto)
    m_spnBatch   ->setValue(s.value(QStringLiteral("gui/batchOverride"),    0).toInt());
    m_spnInFlight->setValue(s.value(QStringLiteral("gui/inFlightOverride"), 0).toInt());
}

void MainWindow::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("gui/provider"),          m_provider->currentText());
    if (m_mode) s.setValue(QStringLiteral("gui/mode"),  m_mode->currentIndex());
    s.setValue(QStringLiteral("gui/srt"),               m_chkSrt->isChecked());
    s.setValue(QStringLiteral("gui/vtt"),               m_chkVtt->isChecked());
    s.setValue(QStringLiteral("gui/json"),              m_chkJson->isChecked());
    s.setValue(QStringLiteral("gui/md"),                m_chkMd->isChecked());
    s.setValue(QStringLiteral("gui/outputDir"),         m_outputDir);
    s.setValue(QStringLiteral("gui/batchOverride"),     m_spnBatch->value());
    s.setValue(QStringLiteral("gui/inFlightOverride"),  m_spnInFlight->value());
}

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
    auto* item = m_model->item(row, ColStatus);
    if (!item) return;
    item->setText(status);

    // Per-status foreground: muted=待处理, phase-blue=解码中/转写中/处理中, green=完成,
    //                        red=失败, amber=取消.
    QColor fg;
    if      (status == tr("待处理")) fg = QColor(0x70, 0x73, 0x7d);  // muted gray
    else if (status == QStringLiteral("解码中")) fg = QColor(0x58, 0xa6, 0xff);  // cyan-blue (phase)
    else if (status == QStringLiteral("转写中")) fg = QColor(0x4a, 0x9e, 0xff);  // accent blue (phase)
    else if (status == tr("处理中")) fg = QColor(0x4a, 0x9e, 0xff);  // accent blue
    else if (status == tr("完成"))   fg = QColor(0x3f, 0xb9, 0x50);  // green
    else if (status == tr("失败"))   fg = QColor(0xf8, 0x51, 0x49);  // red
    else if (status == tr("取消"))   fg = QColor(0xd2, 0x99, 0x22);  // amber
    else                             fg = QColor(0xe6, 0xe6, 0xe6);  // default light

    item->setForeground(QBrush(fg));
}

void MainWindow::setRowSegments(int row, int segs)
{
    if (auto* item = m_model->item(row, ColSegs))
        item->setText(QString::number(segs));
}

void MainWindow::setRowProgress(int row, int percent)
{
    if (auto* item = m_model->item(row, ColProgress)) {
        item->setData(percent, ProgressRole);
        // Trigger a repaint of just this cell (the delegate reads ProgressRole).
        const QModelIndex idx = m_model->index(row, ColProgress);
        emit m_model->dataChanged(idx, idx, {ProgressRole, Qt::DisplayRole});
    }
}

void MainWindow::setRowError(int row, const QString& err)
{
    if (auto* item = m_model->item(row, ColErr))
        item->setText(err);
}

// ---------------------------------------------------------------------------
// Headless test hooks
// ---------------------------------------------------------------------------
void MainWindow::testStart(const QString& file)
{
    addInputFile(file);
    onStart();
}

void MainWindow::testAddRow(const QString& path)
{
    addInputFile(path);   // populate the table without launching the worker
}

QString MainWindow::testRowStatus(int row) const
{
    if (auto* item = m_model->item(row, ColStatus))
        return item->text();
    return QString();
}

int MainWindow::testRowProgress(int row) const
{
    if (auto* item = m_model->item(row, ColProgress))
        return item->data(ProgressRole).toInt();
    return -1;
}

QString MainWindow::testStatusText() const
{
    return m_statusLabel ? m_statusLabel->text() : QString();
}

void MainWindow::testCancel()
{
    onCancel();
}

QString MainWindow::testLogText() const
{
    return m_log ? m_log->toPlainText() : QString();
}

// ---------------------------------------------------------------------------
// C2: Log panel slot — timestamped, color-coded rich-text append.
// Auto-scrolls to the newest line only when the scrollbar is already at (or
// very near) the bottom so a user who scrolled up to read isn't yanked back.
// ---------------------------------------------------------------------------
void MainWindow::appendLogLine(QString level, QString msg)
{
    if (!m_log) return;

    const QString ts = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    const QString html = logLineHtml(level, msg, ts);

    // Check if scrollbar is near the bottom BEFORE appending (append moves it).
    QScrollBar* vsb = m_log->verticalScrollBar();
    const bool atBottom = !vsb || (vsb->value() >= vsb->maximum() - 4);

    m_log->append(html);   // QTextEdit::append adds the HTML as a new paragraph

    if (atBottom && vsb)
        vsb->setValue(vsb->maximum());
}

} // namespace suji
