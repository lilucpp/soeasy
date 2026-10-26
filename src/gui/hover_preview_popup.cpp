#include "gui/hover_preview_popup.h"

#include <QDateTime>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QScreen>
#include <QVBoxLayout>

namespace soeasy {
namespace gui {

namespace {

QString FormatFileSize(uint64_t bytes) {
  if (bytes < 1024) {
    return QString("%1 B").arg(bytes);
  }
  if (bytes < 1024 * 1024) {
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
  }
  return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

}  // namespace

HoverPreviewPopup::HoverPreviewPopup(QWidget* parent)
    : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint) {
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TranslucentBackground);

  auto* root_layout = new QVBoxLayout(this);
  root_layout->setContentsMargins(8, 8, 8, 8);

  auto* card = new QWidget(this);
  card->setObjectName("PreviewCard");
  card->setStyleSheet(
      "#PreviewCard {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #B0BEC5;"
      "  border-radius: 8px;"
      "}");

  auto* card_layout = new QVBoxLayout(card);
  card_layout->setContentsMargins(12, 10, 12, 10);
  card_layout->setSpacing(6);

  title_label_ = new QLabel(card);
  title_label_->setStyleSheet("font-size: 13px; font-weight: bold; color: #1565C0;");

  path_label_ = new QLabel(card);
  path_label_->setStyleSheet("font-size: 11px; color: #78909C;");
  path_label_->setWordWrap(true);

  snippet_label_ = new QLabel(card);
  snippet_label_->setTextFormat(Qt::RichText);
  snippet_label_->setStyleSheet(
      "font-size: 12px; color: #263238; line-height: 1.4; background-color: #F5F7F8; "
      "border-radius: 4px; padding: 6px;");
  snippet_label_->setWordWrap(true);

  meta_label_ = new QLabel(card);
  meta_label_->setStyleSheet("font-size: 11px; color: #90A4AE;");

  card_layout->addWidget(title_label_);
  card_layout->addWidget(path_label_);
  card_layout->addWidget(snippet_label_);
  card_layout->addWidget(meta_label_);

  root_layout->addWidget(card);

  auto* shadow = new QGraphicsDropShadowEffect(this);
  shadow->setBlurRadius(16);
  shadow->setColor(QColor(0, 0, 0, 60));
  shadow->setOffset(0, 4);
  card->setGraphicsEffect(shadow);

  setFixedWidth(460);
}

void HoverPreviewPopup::ShowSnippet(const QPoint& global_pos, const QString& title,
                                    const QString& path, const QString& snippet_html,
                                    uint64_t file_size, int64_t mtime, double score) {
  title_label_->setText(title);
  path_label_->setText(path);

  // Style <mark> tags inside snippet
  QString styled_snippet = snippet_html;
  styled_snippet.replace(
      "<mark>",
      "<span style=\"background-color: #FFF59D; color: #C2185B; font-weight: bold; border-radius: 2px;\">");
  styled_snippet.replace("</mark>", "</span>");

  if (styled_snippet.isEmpty()) {
    snippet_label_->setText("<i>(未提取到上下文摘要或文件为空)</i>");
  } else {
    // Preserve linebreaks in HTML and wrap in container
    styled_snippet.replace("\r\n", "<br/>");
    styled_snippet.replace("\n", "<br/>");
    styled_snippet = QString("<div style=\"line-height: 1.4;\">%1</div>").arg(styled_snippet);
    snippet_label_->setText(styled_snippet);
  }

  QString date_str =
      QDateTime::fromSecsSinceEpoch(mtime).toString("yyyy-MM-dd hh:mm");
  meta_label_->setText(QString("大小: %1  |  修改时间: %2  |  相关度得分: %3")
                           .arg(FormatFileSize(file_size))
                           .arg(date_str)
                           .arg(QString::number(score, 'f', 2)));

  adjustSize();

  // Position smartly near cursor without overflowing screen
  QPoint target_pos = global_pos + QPoint(16, 16);
  QScreen* screen = QGuiApplication::screenAt(global_pos);
  if (screen) {
    QRect screen_geom = screen->availableGeometry();
    if (target_pos.x() + width() > screen_geom.right()) {
      target_pos.setX(global_pos.x() - width() - 8);
    }
    if (target_pos.y() + height() > screen_geom.bottom()) {
      target_pos.setY(screen_geom.bottom() - height() - 8);
    }
  }

  move(target_pos);
  show();
}

void HoverPreviewPopup::HidePopup() {
  hide();
}

}  // namespace gui
}  // namespace soeasy
