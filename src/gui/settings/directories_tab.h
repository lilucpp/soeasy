// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_DIRECTORIES_TAB_H_
#define SOEASY_SRC_GUI_SETTINGS_DIRECTORIES_TAB_H_

#include <QLabel>
#include <QListWidget>
#include <QPushButton>

#include "gui/settings/isettings_tab.h"

namespace soeasy {
namespace gui {

class DirectoriesTab : public ISettingsTab {
  Q_OBJECT

 public:
  explicit DirectoriesTab(QWidget *parent = nullptr);
  ~DirectoriesTab() override = default;

  void PopulateFromConfig(const service::IndexConfig &config) override;
  void ApplyToConfig(service::IndexConfig *config) override;
  void RetranslateUi() override;

 private slots:
  void OnAddDirectory();
  void OnRemoveDirectory();

 private:
  void SetupUi();

  QLabel *label_dirs_ = nullptr;
  QListWidget *dirs_list_ = nullptr;
  QPushButton *btn_add_dir_ = nullptr;
  QPushButton *btn_rm_dir_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_DIRECTORIES_TAB_H_
