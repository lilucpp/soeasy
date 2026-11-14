// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_GENERAL_TAB_H_
#define SOEASY_SRC_GUI_SETTINGS_GENERAL_TAB_H_

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>

#include "gui/settings/isettings_tab.h"

namespace soeasy {
namespace gui {

/**
 * @brief Consolidated General & Indexing Scope tab: UI language, watched directories, and exclusion filters.
 */
class GeneralTab : public ISettingsTab {
  Q_OBJECT

 public:
  explicit GeneralTab(QWidget *parent = nullptr);
  ~GeneralTab() override = default;

  void PopulateFromConfig(const service::IndexConfig &config) override;
  void ApplyToConfig(service::IndexConfig *config) override;
  void RetranslateUi() override;
  void SetStructuralReadOnly(bool read_only) override;

 signals:
  void LanguageChanged(const std::string &lang);

 private slots:
  void OnAddDirectory();
  void OnRemoveDirectory();
  void OnAddPattern();
  void OnRemovePattern();

 private:
  void SetupUi();

  QGroupBox *group_appearance_ = nullptr;
  QLabel *label_language_ = nullptr;
  QComboBox *language_combo_ = nullptr;
  QLabel *label_lang_hint_ = nullptr;

  QGroupBox *group_dirs_ = nullptr;
  QLabel *label_dirs_ = nullptr;
  QListWidget *dirs_list_ = nullptr;
  QPushButton *btn_add_dir_ = nullptr;
  QPushButton *btn_rm_dir_ = nullptr;

  QGroupBox *group_exclusions_ = nullptr;
  QLabel *label_patterns_ = nullptr;
  QListWidget *patterns_list_ = nullptr;
  QLineEdit *new_pattern_edit_ = nullptr;
  QPushButton *btn_add_pattern_ = nullptr;
  QPushButton *btn_rm_pattern_ = nullptr;
  QCheckBox *filter_thumbs_check_ = nullptr;
  bool structural_read_only_ = false;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_GENERAL_TAB_H_
