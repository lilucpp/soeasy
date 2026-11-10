// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_RESULT_ITEM_DELEGATE_H_
#define SOEASY_SRC_GUI_RESULT_ITEM_DELEGATE_H_

#include <QColor>
#include <QPainter>
#include <QStyledItemDelegate>

namespace soeasy {
namespace gui {

class ResultItemDelegate : public QStyledItemDelegate {
  Q_OBJECT

 public:
  explicit ResultItemDelegate(QObject *parent = nullptr);

  void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

  void SetCurrentQuery(const QString &query) { current_query_ = query; }

 private:
  void DrawTypeBadge(QPainter *painter, const QRect &rect, const QString &ext) const;
  void DrawHighlightedText(QPainter *painter, const QRect &rect, const QString &text, const QString &query,
                           const QColor &text_color, bool bold) const;

  QString current_query_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_RESULT_ITEM_DELEGATE_H_
