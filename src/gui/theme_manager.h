// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_THEME_MANAGER_H_
#define SOEASY_SRC_GUI_THEME_MANAGER_H_

#include <QApplication>
#include <QString>

namespace soeasy {
namespace gui {

enum class ThemeMode {
  Light,
  Dark,
};

class ThemeManager {
 public:
  static ThemeManager &Instance();

  // Initialize and apply theme to application instance
  void Init(QApplication *app);

  // Switch theme dynamically at runtime
  void SetTheme(ThemeMode mode);

  ThemeMode CurrentTheme() const { return current_mode_; }

 private:
  ThemeManager() = default;
  ~ThemeManager() = default;
  ThemeManager(const ThemeManager &) = delete;
  ThemeManager &operator=(const ThemeManager &) = delete;

  QString LoadStyleSheet(const QString &resource_path);

  QApplication *app_ = nullptr;
  ThemeMode current_mode_ = ThemeMode::Light;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_THEME_MANAGER_H_
