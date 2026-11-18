// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_GUI_UTILS_H_
#define SOEASY_SRC_GUI_GUI_UTILS_H_

#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QString>
#include <algorithm>
#include <cstdint>

namespace soeasy {
namespace gui {

/**
 * @brief Common pure utility functions for GUI presentation and formatting.
 */
inline QString FormatFileSize(uint64_t bytes) {
  if (bytes < 1024) {
    return QString("%1 B").arg(bytes);
  }
  if (bytes < 1024 * 1024) {
    return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
  }
  return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

inline QString FormatTimestamp(int64_t epoch_sec, const QString &format = "yyyy-MM-dd hh:mm") {
  if (epoch_sec <= 0) return QString("-");
  return QDateTime::fromSecsSinceEpoch(epoch_sec).toString(format);
}

inline QString GenerateKbdBadgeHtml(const QString &key) {
  return QString("<span style=\"background-color:#E2E8F0; color:#0F172A; font-family:monospace; "
                 "font-size:11px; font-weight:bold; border-radius:4px;\">&nbsp;%1&nbsp;</span>")
      .arg(key);
}

/**
 * @brief Single-line QLabel that elides overflowing text without inflating parent layout minimum width.
 */
class ElidedLabel : public QLabel {
  Q_OBJECT

 public:
  explicit ElidedLabel(Qt::TextElideMode elide_mode = Qt::ElideRight, bool stretch_horizontally = true,
                       QWidget *parent = nullptr)
      : QLabel(parent), elide_mode_(elide_mode) {
    setWordWrap(false);
    setMinimumWidth(0);
    setSizePolicy(stretch_horizontally ? QSizePolicy::Ignored : QSizePolicy::Preferred, QSizePolicy::Preferred);
  }

  void SetFullText(const QString &text, bool enable_tooltip = true) {
    full_text_ = text;
    setToolTip(enable_tooltip ? text : QString());
    updateGeometry();
    UpdateElidedText();
  }

  [[nodiscard]] const QString &full_text() const { return full_text_; }

  QSize minimumSizeHint() const override {
    QSize hint = QLabel::minimumSizeHint();
    hint.setWidth(0);
    return hint;
  }

  QSize sizeHint() const override {
    const QFontMetrics fm(font());
    const int text_w = std::max(fm.horizontalAdvance(full_text_), fm.boundingRect(full_text_).width());
    const int frame_w = std::max(0, width() - contentsRect().width());
    // Use generous fallback padding before first layout so QSS padded pills never clip short text by 1-2px.
    const int pad_w = (frame_w > 0 ? frame_w : 18) + 4;
    const int base_h = QLabel::sizeHint().height();
    return QSize(text_w + pad_w, base_h);
  }

 protected:
  void resizeEvent(QResizeEvent *event) override {
    QLabel::resizeEvent(event);
    UpdateElidedText();
  }

  void changeEvent(QEvent *event) override {
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
      updateGeometry();
      UpdateElidedText();
    }
  }

 private:
  void UpdateElidedText() {
    const int avail = contentsRect().width();
    const QString display = (avail > 0) ? QFontMetrics(font()).elidedText(full_text_, elide_mode_, avail) : full_text_;
    if (text() != display) {
      QLabel::setText(display);
    }
  }

  QString full_text_;
  Qt::TextElideMode elide_mode_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_GUI_UTILS_H_
