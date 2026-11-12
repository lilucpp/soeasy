// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_THEME_MANAGER_H_
#define SOEASY_SRC_GUI_THEME_MANAGER_H_

#include <QApplication>
#include <QFile>
#include <QString>
#include <QTextStream>

namespace soeasy {
namespace gui {

class ThemeManager {
 public:
  static void ApplyDefaultTheme(QApplication &app) {
    QFile file(":/styles/theme_light.qss");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      QTextStream stream(&file);
      app.setStyleSheet(stream.readAll());
    }
  }

  static QString GetDefaultStylesheet() {
    QFile file(":/styles/theme_light.qss");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      QTextStream stream(&file);
      return stream.readAll();
    }
    return QString();
  }
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_THEME_MANAGER_H_
