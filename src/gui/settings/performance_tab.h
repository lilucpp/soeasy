// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_SETTINGS_PERFORMANCE_TAB_H_
#define SOEASY_SRC_GUI_SETTINGS_PERFORMANCE_TAB_H_

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

#include "gui/settings/isettings_tab.h"

namespace soeasy {
namespace gui {

class PerformanceTab : public ISettingsTab {
  Q_OBJECT

 public:
  explicit PerformanceTab(QWidget *parent = nullptr);
  ~PerformanceTab() override = default;

  void PopulateFromConfig(const service::IndexConfig &config) override;
  void ApplyToConfig(service::IndexConfig *config) override;
  void RetranslateUi() override;

 private slots:
  void OnBrowseDocToTextLogPath();

 private:
  void SetupUi();

  QLabel *label_debounce_ = nullptr;
  QSpinBox *debounce_spin_ = nullptr;
  QLabel *label_max_size_ = nullptr;
  QSpinBox *max_size_spin_ = nullptr;
  QCheckBox *filter_thumbs_check_ = nullptr;

  QLabel *label_log_level_ = nullptr;
  QComboBox *log_level_combo_ = nullptr;
  QCheckBox *log_console_check_ = nullptr;
  QCheckBox *log_sync_check_ = nullptr;
  QCheckBox *doctotext_log_check_ = nullptr;
  QLabel *label_dt_path_ = nullptr;
  QLineEdit *doctotext_log_path_edit_ = nullptr;
  QPushButton *dt_browse_btn_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SETTINGS_PERFORMANCE_TAB_H_
