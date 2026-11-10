#include "gui/title_bar.h"

#include "gui/i18n.h"
#include "gui/icon_provider.h"

namespace soeasy {
namespace gui {

TitleBar::TitleBar(QWidget *parent) : QWidget(parent) {
  setFixedHeight(48);
  SetupUi();
}

void TitleBar::SetupUi() {
  setObjectName("TitleBar");

  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(16, 0, 16, 0);
  layout->setSpacing(10);

  icon_label_ = new QLabel(this);
  icon_label_->setObjectName("BrandLogo");
  icon_label_->setFixedSize(24, 24);
  icon_label_->setAlignment(Qt::AlignCenter);
  icon_label_->setPixmap(IconProvider::GetAppIcon().pixmap(24, 24));

  title_label_ = new QLabel("SoEasy", this);
  title_label_->setObjectName("BrandTitle");

  divider_ = new QFrame(this);
  divider_->setObjectName("BrandDivider");
  divider_->setFrameShape(QFrame::VLine);
  divider_->setFrameShadow(QFrame::Plain);

  slogan_label_ = new QLabel(this);
  slogan_label_->setObjectName("BrandSlogan");
  slogan_label_->setText("让电脑没有难搜的文件");

  btn_min_ = new QPushButton("—", this);
  btn_min_->setObjectName("BtnMinimize");
  btn_min_->setProperty("role", "win-control");
  btn_min_->setToolTip("最小化");
  btn_min_->setFixedSize(32, 28);

  btn_max_ = new QPushButton("□", this);
  btn_max_->setObjectName("BtnMaximize");
  btn_max_->setProperty("role", "win-control");
  btn_max_->setToolTip("最大化 / 还原");
  btn_max_->setFixedSize(32, 28);

  btn_close_ = new QPushButton("✕", this);
  btn_close_->setObjectName("BtnClose");
  btn_close_->setProperty("role", "win-control");
  btn_close_->setProperty("role", "win-close");
  btn_close_->setToolTip("关闭");
  btn_close_->setFixedSize(34, 28);

  layout->addWidget(icon_label_);
  layout->addWidget(title_label_);
  layout->addWidget(divider_);
  layout->addWidget(slogan_label_);
  layout->addStretch(1);
  layout->addWidget(btn_min_);
  layout->addWidget(btn_max_);
  layout->addWidget(btn_close_);

  connect(btn_min_, &QPushButton::clicked, this, &TitleBar::OnMinimizeClicked);
  connect(btn_max_, &QPushButton::clicked, this, &TitleBar::OnMaximizeClicked);
  connect(btn_close_, &QPushButton::clicked, this, &TitleBar::OnCloseClicked);

  RetranslateUi();
}

void TitleBar::SetSubtitle(const QString &subtitle) {
  if (slogan_label_) {
    slogan_label_->setText(subtitle);
  }
}

void TitleBar::RetranslateUi() {
  if (title_label_) {
    title_label_->setText("SoEasy");
  }
  if (slogan_label_ && slogan_label_->text().isEmpty()) {
    slogan_label_->setText(Tr("app_title"));
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

void TitleBar::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    drag_pos_ = event->globalPos() - window()->frameGeometry().topLeft();
    event->accept();
  }
}

void TitleBar::mouseMoveEvent(QMouseEvent *event) {
  if ((event->buttons() & Qt::LeftButton) && !window()->isMaximized()) {
    window()->move(event->globalPos() - drag_pos_);
    event->accept();
  }
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    OnMaximizeClicked();
    event->accept();
  }
}

}  // namespace gui
}  // namespace soeasy
