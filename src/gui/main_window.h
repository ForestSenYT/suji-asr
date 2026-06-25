#pragma once
#include <QMainWindow>
namespace suji {
class MainWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QWidget* parent=nullptr);
};
}
