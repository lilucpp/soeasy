#ifndef SOEASY_SRC_GUI_TITLE_BAR_H_
#define SOEASY_SRC_GUI_TITLE_BAR_H_

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPoint>
#include <QPushButton>
#include <QWidget>

namespace soeasy {
namespace gui {

class TitleBar : public QWidget {
  Q_OBJECT

 public:
  explicit TitleBar(QWidget* parent = nullptr);
  ~TitleBar() override = default;

  void UpdateMaximizeButton(bool is_maximized);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

 private slots:
  void OnMinimizeClicked();
  void OnMaximizeClicked();
  void OnCloseClicked();

 private:
  void SetupUi();

  QLabel* icon_label_ = nullptr;
  QLabel* title_label_ = nullptr;
  QPushButton* btn_min_ = nullptr;
  QPushButton* btn_max_ = nullptr;
  QPushButton* btn_close_ = nullptr;

  QPoint drag_pos_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_TITLE_BAR_H_
