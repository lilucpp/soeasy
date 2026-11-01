#include "gui/title_bar.h"
#include "gui/icon_provider.h"
#include "gui/i18n.h"

namespace soeasy {
namespace gui {

TitleBar::TitleBar(QWidget* parent) : QWidget(parent) {
  setFixedHeight(36);
  SetupUi();
}

void TitleBar::SetupUi() {
  setObjectName("TitleBar");
  setStyleSheet(
      "#TitleBar {"
      "  background-color: #FFFAFA;"
      "  border: none;"
      "}");

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(12, 0, 0, 0);
  layout->setSpacing(8);

  icon_label_ = new QLabel(this);
  icon_label_->setFixedSize(20, 20);
  icon_label_->setAlignment(Qt::AlignCenter);
  icon_label_->setPixmap(IconProvider::GetAppIcon().pixmap(18, 18));
  icon_label_->setStyleSheet("border: none; background: transparent; padding: 0px;");

  title_label_ = new QLabel("SoEasy - 让电脑没有难搜的文件", this);
  title_label_->setStyleSheet(
      "QLabel {"
      "  font-size: 13px;"
      "  font-weight: bold;"
      "  color: #1E293B;"
      "  background: transparent;"
      "  border: none;"
      "}");

  btn_min_ = new QPushButton("—", this);
  btn_min_->setToolTip("最小化");
  btn_min_->setFixedSize(40, 36);
  btn_min_->setStyleSheet(
      "QPushButton {"
      "  border: none;"
      "  background: transparent;"
      "  color: #475569;"
      "  font-size: 13px;"
      "  font-weight: bold;"
      "}"
      "QPushButton:hover {"
      "  background-color: #DCE8F5;"
      "  color: #1E293B;"
      "  border-radius: 4px;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #CBDCEF;"
      "}");

  btn_max_ = new QPushButton("□", this);
  btn_max_->setToolTip("最大化 / 还原");
  btn_max_->setFixedSize(40, 36);
  btn_max_->setStyleSheet(
      "QPushButton {"
      "  border: none;"
      "  background: transparent;"
      "  color: #475569;"
      "  font-size: 13px;"
      "}"
      "QPushButton:hover {"
      "  background-color: #DCE8F5;"
      "  color: #1E293B;"
      "  border-radius: 4px;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #CBDCEF;"
      "}");

  btn_close_ = new QPushButton("✕", this);
  btn_close_->setToolTip("关闭");
  btn_close_->setFixedSize(44, 36);
  btn_close_->setStyleSheet(
      "QPushButton {"
      "  border: none;"
      "  background: transparent;"
      "  color: #475569;"
      "  font-size: 13px;"
      "}"
      "QPushButton:hover {"
      "  background-color: #EF4444;"
      "  color: #FFFFFF;"
      "  border-radius: 4px;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #DC2626;"
      "  color: #FFFFFF;"
      "}");

  layout->addWidget(icon_label_);
  layout->addWidget(title_label_);
  layout->addStretch(1);
  layout->addWidget(btn_min_);
  layout->addWidget(btn_max_);
  layout->addWidget(btn_close_);

  connect(btn_min_, &QPushButton::clicked, this, &TitleBar::OnMinimizeClicked);
  connect(btn_max_, &QPushButton::clicked, this, &TitleBar::OnMaximizeClicked);
  connect(btn_close_, &QPushButton::clicked, this, &TitleBar::OnCloseClicked);

  RetranslateUi();
}

void TitleBar::RetranslateUi() {
  if (title_label_) {
    title_label_->setText(Tr("app_title"));
  }
  if (btn_min_) {
    btn_min_->setToolTip(Tr("minimize"));
  }
  if (btn_max_) {
    btn_max_->setToolTip(Tr("maximize"));
  }
  if (btn_close_) {
    btn_close_->setToolTip(Tr("close"));
  }
}

void TitleBar::UpdateMaximizeButton(bool is_maximized) {
  if (is_maximized) {
    btn_max_->setText("❐");
    btn_max_->setToolTip("还原");
  } else {
    btn_max_->setText("□");
    btn_max_->setToolTip("最大化");
  }
}

void TitleBar::OnMinimizeClicked() {
  if (window()) {
    window()->showMinimized();
  }
}

void TitleBar::OnMaximizeClicked() {
  if (!window()) return;
  if (window()->isMaximized()) {
    window()->showNormal();
    UpdateMaximizeButton(false);
  } else {
    window()->showMaximized();
    UpdateMaximizeButton(true);
  }
}

void TitleBar::OnCloseClicked() {
  if (window()) {
    window()->close();
  }
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    drag_pos_ = event->globalPos() - window()->frameGeometry().topLeft();
    event->accept();
  }
}

void TitleBar::mouseMoveEvent(QMouseEvent* event) {
  if ((event->buttons() & Qt::LeftButton) && !window()->isMaximized()) {
    window()->move(event->globalPos() - drag_pos_);
    event->accept();
  }
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    OnMaximizeClicked();
    event->accept();
  }
}

}  // namespace gui
}  // namespace soeasy
