// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/theme_manager.h"

#include <QFile>
#include <QTextStream>
#include <iostream>

int qInitResources_resources();

namespace soeasy {
namespace gui {

ThemeManager &ThemeManager::Instance() {
  static ThemeManager s_instance;
  return s_instance;
}

void ThemeManager::Init(QApplication *app) {
  ::qInitResources_resources();
  app_ = app;
  SetTheme(ThemeMode::Light);
}

void ThemeManager::SetTheme(ThemeMode mode) {
  current_mode_ = mode;
  if (!app_) {
    return;
  }

  QString path = ":/styles/theme_light.qss";
  if (mode == ThemeMode::Dark) {
    // If dark theme is available in future iterations
    path = ":/styles/theme_dark.qss";
  }

  QString qss = LoadStyleSheet(path);
  if (qss.isEmpty() && mode == ThemeMode::Dark) {
    // Fallback to light
    qss = LoadStyleSheet(":/styles/theme_light.qss");
  }

  if (!qss.isEmpty()) {
    app_->setStyleSheet(qss);
  }
}

QString ThemeManager::LoadStyleSheet(const QString &resource_path) {
  QFile file(resource_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    std::cerr << "[-] [ThemeManager] Failed to open stylesheet: " << resource_path.toStdString() << "\n";
    return QString();
  }
  QTextStream in(&file);
  QString content = in.readAll();
  std::cout << "[+] [ThemeManager] Loaded stylesheet (" << content.size() << " bytes) from " << resource_path.toStdString() << "\n";
  return content;
}

}  // namespace gui
}  // namespace soeasy
