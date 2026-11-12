// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_PATTERNS_TAB_H_
#define SOEASY_SRC_GUI_SETTINGS_PATTERNS_TAB_H_

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>

#include "gui/settings/isettings_tab.h"

namespace soeasy {
namespace gui {

class PatternsTab : public ISettingsTab {
  Q_OBJECT

 public:
  explicit PatternsTab(QWidget *parent = nullptr);
  ~PatternsTab() override = default;

  void PopulateFromConfig(const service::IndexConfig &config) override;
  void ApplyToConfig(service::IndexConfig *config) override;
  void RetranslateUi() override;

 private slots:
  void OnAddPattern();
  void OnRemovePattern();

 private:
  void SetupUi();

  QLabel *label_patterns_ = nullptr;
  QListWidget *patterns_list_ = nullptr;
  QLineEdit *new_pattern_edit_ = nullptr;
  QPushButton *btn_add_pattern_ = nullptr;
  QPushButton *btn_rm_pattern_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_PATTERNS_TAB_H_
