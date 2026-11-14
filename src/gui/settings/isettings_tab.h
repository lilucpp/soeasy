// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_ISETTINGS_TAB_H_
#define SOEASY_SRC_GUI_SETTINGS_ISETTINGS_TAB_H_

#include <QWidget>

namespace soeasy {
namespace service {
class IndexConfig;
}
namespace gui {

/**
 * @brief Common interface for SettingsDialog sub-page tabs.
 */
class ISettingsTab : public QWidget {
 public:
  explicit ISettingsTab(QWidget *parent = nullptr) : QWidget(parent) {}
  ~ISettingsTab() override = default;

  virtual void PopulateFromConfig(const service::IndexConfig &config) = 0;
  virtual void ApplyToConfig(service::IndexConfig *config) = 0;
  virtual void RetranslateUi() = 0;
  virtual void SetStructuralReadOnly(bool /*read_only*/) {}
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_ISETTINGS_TAB_H_
