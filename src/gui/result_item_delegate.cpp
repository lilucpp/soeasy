// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/result_item_delegate.h"

#include <QFileInfo>
#include <QFontMetrics>
#include <QRegularExpression>

#include "gui/search_table_model.h"

namespace soeasy {
namespace gui {

ResultItemDelegate::ResultItemDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize ResultItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const {
  QSize s = QStyledItemDelegate::sizeHint(option, index);
  s.setHeight(54);
  return s;
}

void ResultItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const {
  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->setRenderHint(QPainter::TextAntialiasing, true);

  const bool is_selected = (option.state & QStyle::State_Selected);
  const bool is_hover = (option.state & QStyle::State_MouseOver);

  // 1. Draw Row Background
  if (is_selected) {
    painter->fillRect(option.rect, QColor("#EFF6FF"));
  } else if (is_hover) {
    painter->fillRect(option.rect, QColor("#F8FAFC"));
  } else {
    painter->fillRect(option.rect, QColor("#FFFFFF"));
  }

  // 1.1 Bottom Subtle Divider line
  painter->setPen(QColor("#F1F5F9"));
  painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());

  // 1.2 Draw Left Active Indicator Bar (3px solid blue) on column 1 or first visible column
  if (is_selected && index.column() == SearchTableModel::kColFileName) {
    painter->fillRect(QRect(option.rect.left(), option.rect.top() + 4, 3, option.rect.height() - 8), QColor("#2563EB"));
  }

  const int col = index.column();

  if (col == SearchTableModel::kColFileName) {
    // Left file icon badge + 2-line title & path
    QString file_name = index.data(Qt::DisplayRole).toString();
    QString path = index.sibling(index.row(), SearchTableModel::kColPath).data(Qt::DisplayRole).toString();
    QString ext = QFileInfo(file_name).suffix().toLower();

    // Type Badge Rect (30x24)
    int badge_x = option.rect.left() + 16;
    int badge_y = option.rect.top() + (option.rect.height() - 24) / 2;
    QRect badge_rect(badge_x, badge_y, 32, 24);
    DrawTypeBadge(painter, badge_rect, ext);

    // Text Content Area (Filename top, Path bottom)
    int text_x = badge_rect.right() + 12;
    int text_w = option.rect.right() - text_x - 12;
    if (text_w > 0) {
      // Line 1: Bold Filename
      QRect name_rect(text_x, option.rect.top() + 8, text_w, 18);
      DrawHighlightedText(painter, name_rect, file_name, current_query_,
                          is_selected ? QColor("#2563EB") : QColor("#0F172A"), true);

      // Line 2: Breadcrumb Path (e.g. notes / tools / google)
      QString breadcrumb = path;
      breadcrumb.replace("\\", "/");
      if (breadcrumb.startsWith("/")) breadcrumb.remove(0, 1);
      breadcrumb.replace("/", " / ");

      QFont path_font = painter->font();
      path_font.setBold(false);
      path_font.setPointSize(9);
      painter->setFont(path_font);
      painter->setPen(QColor("#64748B"));

      QRect path_rect(text_x, option.rect.top() + 29, text_w, 16);
      QFontMetrics fm(path_font);
      QString elided_path = fm.elidedText(breadcrumb, Qt::ElideRight, text_w);
      painter->drawText(path_rect, Qt::AlignLeft | Qt::AlignVCenter, elided_path);
    }
  } else if (col == SearchTableModel::kColSize || col == SearchTableModel::kColMtime) {
    // Secondary text column (Size or Mtime)
    QString val = index.data(Qt::DisplayRole).toString();
    QFont f = painter->font();
    f.setPointSize(9);
    f.setBold(false);
    painter->setFont(f);
    painter->setPen(QColor("#64748B"));

    QRect r = option.rect.adjusted(8, 0, -14, 0);
    Qt::Alignment align = (col == SearchTableModel::kColSize) ? (Qt::AlignRight | Qt::AlignVCenter)
                                                               : (Qt::AlignLeft | Qt::AlignVCenter);
    painter->drawText(r, align, val);
  } else {
    // Fallback for default column painting if any
    QStyledItemDelegate::paint(painter, option, index);
  }

  painter->restore();
}

void ResultItemDelegate::DrawTypeBadge(QPainter *painter, const QRect &rect, const QString &ext) const {
  QColor bg_color("#E2E8F0");
  QColor text_color("#475569");
  QString label = ext.toUpper();

  if (ext == "sh" || ext == "bash" || ext == "zsh") {
    bg_color = QColor("#334155");
    text_color = QColor("#FFFFFF");
    label = "SH";
  } else if (ext == "md" || ext == "markdown") {
    bg_color = QColor("#F1F5F9");
    text_color = QColor("#475569");
    label = "MD";
  } else if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp") {
    bg_color = QColor("#E0F2FE");
    text_color = QColor("#0284C7");
    label = "C++";
  } else if (ext == "html" || ext == "htm" || ext == "xml") {
    bg_color = QColor("#FFEDD5");
    text_color = QColor("#EA580C");
    label = "<>";
  } else if (ext == "pdf") {
    bg_color = QColor("#FEE2E2");
    text_color = QColor("#DC2626");
    label = "PDF";
  } else if (ext == "doc" || ext == "docx") {
    bg_color = QColor("#DBEAFE");
    text_color = QColor("#2563EB");
    label = "DOC";
  } else if (ext == "xls" || ext == "xlsx") {
    bg_color = QColor("#DCFCE7");
    text_color = QColor("#16A34A");
    label = "XLS";
  } else if (ext == "ppt" || ext == "pptx") {
    bg_color = QColor("#FFEDD5");
    text_color = QColor("#D97706");
    label = "PPT";
  } else if (ext == "txt" || ext == "log") {
    bg_color = QColor("#F8FAFC");
    text_color = QColor("#64748B");
    label = "TXT";
  } else if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp") {
    bg_color = QColor("#F3E8FF");
    text_color = QColor("#9333EA");
    label = "IMG";
  }

  painter->save();
  painter->setPen(QColor(0, 0, 0, 15));
  painter->setBrush(bg_color);
  painter->drawRoundedRect(rect, 4, 4);

  QFont font = painter->font();
  font.setBold(true);
  font.setPointSize(label.length() > 2 ? 7 : 8);
  painter->setFont(font);
  painter->setPen(text_color);
  painter->drawText(rect, Qt::AlignCenter, label);
  painter->restore();
}

void ResultItemDelegate::DrawHighlightedText(QPainter *painter, const QRect &rect, const QString &text,
                                             const QString &query, const QColor &text_color, bool bold) const {
  painter->save();
  QFont font = painter->font();
  font.setBold(bold);
  font.setPointSize(10);
  painter->setFont(font);

  QFontMetrics fm(font);
  QString q_clean = query.trimmed();

  // If no search query or text doesn't contain query, draw normal text
  if (q_clean.isEmpty() || !text.contains(q_clean, Qt::CaseInsensitive)) {
    painter->setPen(text_color);
    QString elided = fm.elidedText(text, Qt::ElideRight, rect.width());
    painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, elided);
    painter->restore();
    return;
  }

  // Draw with highlight
  int start_idx = text.indexOf(q_clean, 0, Qt::CaseInsensitive);
  int query_len = q_clean.length();

  QString pre = text.left(start_idx);
  QString matched = text.mid(start_idx, query_len);
  QString post = text.mid(start_idx + query_len);

  int x = rect.left();
  int y = rect.top() + (rect.height() - fm.height()) / 2 + fm.ascent();

  // Draw pre
  painter->setPen(text_color);
  if (!pre.isEmpty()) {
    painter->drawText(x, y, pre);
    x += fm.horizontalAdvance(pre);
  }

  // Draw matched with amber gold highlight (#FEF08A)
  int match_w = fm.horizontalAdvance(matched);
  QRect match_bg(x - 2, rect.top() + (rect.height() - fm.height()) / 2 - 1, match_w + 4, fm.height() + 2);
  painter->fillRect(match_bg, QColor("#FEF08A"));
  painter->setPen(QColor("#854D0E"));
  painter->drawText(x, y, matched);
  x += match_w;

  // Draw post
  painter->setPen(text_color);
  if (!post.isEmpty() && x < rect.right()) {
    QString elided_post = fm.elidedText(post, Qt::ElideRight, rect.right() - x);
    painter->drawText(x, y, elided_post);
  }

  painter->restore();
}

}  // namespace gui
}  // namespace soeasy
