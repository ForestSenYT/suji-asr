#pragma once
#include <QMainWindow>
#include <QStringList>

class QTableView;
class QStandardItemModel;
class QProgressBar;
class QLabel;
class QComboBox;
class QCheckBox;
class QPushButton;

namespace suji {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Getters for Task 4 wiring
    QStringList inputFiles() const;
    QString     outputDir() const;
    QString     provider() const;   // "auto" | "cpu" | "cuda"
    bool        wantSrt()  const;
    bool        wantVtt()  const;
    bool        wantJson() const;
    bool        wantMd()   const;

    // Called by Task 4 to update progress / status
    void setProgress(int value);
    void setStatusText(const QString& text);
    void setRowStatus(int row, const QString& status);
    void setRowSegments(int row, int segs);
    void setRowError(int row, const QString& err);

public slots:
    void onAddFiles();
    void onAddFolder();
    void onClearList();
    void onChooseOutputDir();
    void onStart();
    void onCancel();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void addInputFile(const QString& path);
    void addFolder(const QString& dir);
    static bool isMediaFile(const QString& path);

    // Widgets
    QTableView*         m_table       = nullptr;
    QStandardItemModel* m_model       = nullptr;
    QProgressBar*       m_progress    = nullptr;
    QLabel*             m_statusLabel = nullptr;
    QComboBox*          m_provider    = nullptr;
    QCheckBox*          m_chkSrt      = nullptr;
    QCheckBox*          m_chkVtt      = nullptr;
    QCheckBox*          m_chkJson     = nullptr;
    QCheckBox*          m_chkMd       = nullptr;
    QPushButton*        m_btnStart    = nullptr;
    QPushButton*        m_btnCancel   = nullptr;
    QLabel*             m_outDirLabel = nullptr;
    QString             m_outputDir;
};

} // namespace suji
