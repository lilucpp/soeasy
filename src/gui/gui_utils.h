// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_GUI_UTILS_H_
#define SOEASY_SRC_GUI_GUI_UTILS_H_

#include <QDateTime>
#include <QString>
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

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_GUI_UTILS_H_
