// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

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

HoverPreviewPopup::HoverPreviewPopup(QWidget *parent) : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint) {
  setObjectName("HoverPreviewPopup");
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TranslucentBackground);

  auto *root_layout = new QVBoxLayout(this);
  root_layout->setContentsMargins(10, 10, 10, 10);

  auto *card = new QWidget(this);
  card->setObjectName("PreviewCard");

  auto *card_layout = new QVBoxLayout(card);
  card_layout->setContentsMargins(16, 14, 16, 14);
  card_layout->setSpacing(8);

  title_label_ = new QLabel(card);
  title_label_->setObjectName("HoverCardTitle");

  path_label_ = new QLabel(card);
  path_label_->setObjectName("HoverCardPath");
  path_label_->setWordWrap(true);

  snippet_label_ = new QLabel(card);
  snippet_label_->setObjectName("SnippetBox");
  snippet_label_->setTextFormat(Qt::RichText);
  snippet_label_->setWordWrap(true);

  meta_label_ = new QLabel(card);
  meta_label_->setObjectName("HoverCardMeta");

  card_layout->addWidget(title_label_);
  card_layout->addWidget(path_label_);
  card_layout->addWidget(snippet_label_);
  card_layout->addWidget(meta_label_);

  root_layout->addWidget(card);

  auto *shadow = new QGraphicsDropShadowEffect(this);
  shadow->setBlurRadius(24);
  shadow->setColor(QColor(0, 0, 0, 36));
  shadow->setOffset(0, 8);
  card->setGraphicsEffect(shadow);

  setFixedWidth(480);
}

void HoverPreviewPopup::ShowSnippet(const QPoint &global_pos, const QString &title, const QString &path,
                                    const QString &snippet_html, uint64_t file_size, int64_t mtime, double score) {
  title_label_->setText(title);
  path_label_->setText(path);

  // Modern Amber / Gold highlight aligned with Figma tokens
  QString styled_snippet = snippet_html;
  styled_snippet.replace(
      "<mark>",
      "<span style=\"background-color: #FEF08A; color: #854D0E; font-weight: 600; padding: 1px 4px; border-radius: "
      "2px;\">");
  styled_snippet.replace("</mark>", "</span>");

  if (styled_snippet.isEmpty()) {
    snippet_label_->setText("<i>(未提取到正文上下文摘要或文件为二进制格式)</i>");
  } else {
    styled_snippet.replace("\r\n", "<br/>");
    styled_snippet.replace("\n", "<br/>");
    styled_snippet = QString("<div style=\"line-height: 1.5;\">%1</div>").arg(styled_snippet);
    snippet_label_->setText(styled_snippet);
  }

  QString date_str = QDateTime::fromSecsSinceEpoch(mtime).toString("yyyy-MM-dd hh:mm");
  meta_label_->setText(QString("大小: %1  ·  修改时间: %2  ·  相关度: %3")
                           .arg(FormatFileSize(file_size))
                           .arg(date_str)
                           .arg(QString::number(score, 'f', 1)));

  adjustSize();

  // Position smartly near cursor with screen border collision detection
  QPoint target_pos = global_pos + QPoint(18, 18);
  QScreen *screen = QGuiApplication::screenAt(global_pos);
  if (screen) {
    QRect screen_geom = screen->availableGeometry();
    if (target_pos.x() + width() > screen_geom.right()) {
      target_pos.setX(global_pos.x() - width() - 10);
    }
    if (target_pos.y() + height() > screen_geom.bottom()) {
      target_pos.setY(screen_geom.bottom() - height() - 10);
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
