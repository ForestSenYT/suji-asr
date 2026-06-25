#include "gui/main_window.h"
#include <QLabel>
namespace suji {
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("suji 批量转写"));
  resize(900, 600);
  auto* placeholder = new QLabel(QStringLiteral("suji — 批量转写(仪表盘开发中)"), this);
  placeholder->setAlignment(Qt::AlignCenter);
  setCentralWidget(placeholder);
}
}
