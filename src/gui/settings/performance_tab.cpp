// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/performance_tab.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "common/resource_profile.h"
#include "gui/i18n.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

PerformanceTab::PerformanceTab(QWidget *parent) : ISettingsTab(parent) {
  SetupUi();
}

void PerformanceTab::SetupUi() {
  auto *outer_layout = new QVBoxLayout(this);
  outer_layout->setContentsMargins(16, 14, 16, 14);
  outer_layout->setSpacing(12);

  // Section 1: Indexing & Resource Policy
  group_perf_strategy_ = new QGroupBox(this);
  auto *form_perf = new QFormLayout(group_perf_strategy_);
  form_perf->setContentsMargins(12, 12, 12, 12);
  form_perf->setSpacing(10);

  debounce_spin_ = new QSpinBox(group_perf_strategy_);
  debounce_spin_->setRange(50, 5000);
  debounce_spin_->setSingleStep(50);
  label_debounce_ = new QLabel(group_perf_strategy_);
  form_perf->addRow(label_debounce_, debounce_spin_);

  max_size_spin_ = new QSpinBox(group_perf_strategy_);
  max_size_spin_->setRange(1, 1024);
  max_size_spin_->setSingleStep(10);
  label_max_size_ = new QLabel(group_perf_strategy_);
  form_perf->addRow(label_max_size_, max_size_spin_);

  workers_spin_ = new QSpinBox(group_perf_strategy_);
  workers_spin_->setRange(1, 16);
  workers_spin_->setSingleStep(1);
  label_workers_ = new QLabel(group_perf_strategy_);
  form_perf->addRow(label_workers_, workers_spin_);

  resource_profile_combo_ = new QComboBox(group_perf_strategy_);
  resource_profile_combo_->addItem(
      "", QString::fromStdString(common::ResourceProfileToString(common::ResourceProfile::kLowMemory)));
  resource_profile_combo_->addItem(
      "", QString::fromStdString(common::ResourceProfileToString(common::ResourceProfile::kLowWrite)));
  label_resource_profile_ = new QLabel(group_perf_strategy_);
  form_perf->addRow(label_resource_profile_, resource_profile_combo_);

  outer_layout->addWidget(group_perf_strategy_);

  // Section 2: Logging & Diagnostics
  group_diagnostics_ = new QGroupBox(this);
  auto *diag_layout = new QVBoxLayout(group_diagnostics_);
  diag_layout->setContentsMargins(12, 12, 12, 12);
  diag_layout->setSpacing(10);

  auto *form_log_level = new QFormLayout();
  form_log_level->setSpacing(10);
  log_level_combo_ = new QComboBox(group_diagnostics_);
  log_level_combo_->addItem("DEBUG", "debug");
  log_level_combo_->addItem("INFO", "info");
  log_level_combo_->addItem("WARN", "warn");
  log_level_combo_->addItem("ERROR", "error");
  label_log_level_ = new QLabel(group_diagnostics_);
  form_log_level->addRow(label_log_level_, log_level_combo_);
  diag_layout->addLayout(form_log_level);

  log_console_check_ = new QCheckBox(group_diagnostics_);
  log_sync_check_ = new QCheckBox(group_diagnostics_);
  doctotext_log_check_ = new QCheckBox(group_diagnostics_);
  diag_layout->addWidget(log_console_check_);
  diag_layout->addWidget(log_sync_check_);
  diag_layout->addWidget(doctotext_log_check_);

  auto *form_dt_path = new QFormLayout();
  form_dt_path->setSpacing(10);
  auto *dt_path_layout = new QHBoxLayout();
  doctotext_log_path_edit_ = new QLineEdit(group_diagnostics_);
  dt_browse_btn_ = new QPushButton(group_diagnostics_);
  dt_browse_btn_->setMinimumWidth(84);
  dt_path_layout->addWidget(doctotext_log_path_edit_);
  dt_path_layout->addWidget(dt_browse_btn_);
  label_dt_path_ = new QLabel(group_diagnostics_);
  form_dt_path->addRow(label_dt_path_, dt_path_layout);
  diag_layout->addLayout(form_dt_path);

  outer_layout->addWidget(group_diagnostics_);
  outer_layout->addStretch();

  connect(dt_browse_btn_, &QPushButton::clicked, this, &PerformanceTab::OnBrowseDocToTextLogPath);

  RetranslateUi();
}

void PerformanceTab::PopulateFromConfig(const service::IndexConfig &config) {
  debounce_spin_->setValue(static_cast<int>(config.debounce_ms()));
  max_size_spin_->setValue(static_cast<int>(config.max_file_size() / (1024 * 1024)));
  workers_spin_->setValue(static_cast<int>(config.worker_count()));
  const QString profile_val = QString::fromStdString(common::ResourceProfileToString(config.resource_profile()));
  const int profile_idx = resource_profile_combo_->findData(profile_val);
  resource_profile_combo_->setCurrentIndex(profile_idx >= 0 ? profile_idx : 0);

  const int idx = log_level_combo_->findData(QString::fromStdString(config.log_level()));
  log_level_combo_->setCurrentIndex(idx >= 0 ? idx : 1);
  log_console_check_->setChecked(config.log_console());
  log_sync_check_->setChecked(config.log_sync());

  doctotext_log_check_->setChecked(config.doctotext_logging());
  doctotext_log_path_edit_->setText(QString::fromStdString(config.doctotext_log_path()));
}

void PerformanceTab::ApplyToConfig(service::IndexConfig *config) {
  if (!config) return;

  config->set_debounce_ms(static_cast<uint32_t>(debounce_spin_->value()));
  config->set_max_file_size(static_cast<uint64_t>(max_size_spin_->value()) * 1024 * 1024);
  config->set_worker_count(static_cast<size_t>(workers_spin_->value()));
  config->set_resource_profile(
      common::ParseResourceProfile(resource_profile_combo_->currentData().toString().toStdString()));

  config->set_log_level(log_level_combo_->currentData().toString().toStdString());
  config->set_log_console(log_console_check_->isChecked());
  config->set_log_sync(log_sync_check_->isChecked());
  config->set_doctotext_logging(doctotext_log_check_->isChecked());
  config->set_doctotext_log_path(doctotext_log_path_edit_->text().trimmed().toStdString());
}

void PerformanceTab::RetranslateUi() {
  if (group_perf_strategy_) group_perf_strategy_->setTitle(Tr("group_perf_strategy"));
  if (label_debounce_) label_debounce_->setText(Tr("label_debounce"));
  if (debounce_spin_) debounce_spin_->setSuffix(Tr("suffix_ms"));
  if (label_max_size_) label_max_size_->setText(Tr("label_max_size"));
  if (max_size_spin_) max_size_spin_->setSuffix(Tr("suffix_mb"));
  if (label_workers_) label_workers_->setText(Tr("label_workers"));
  if (label_resource_profile_) label_resource_profile_->setText(Tr("label_resource_profile"));
  if (resource_profile_combo_) {
    resource_profile_combo_->setItemText(0, Tr("profile_low_memory"));
    resource_profile_combo_->setItemText(1, Tr("profile_low_write"));
    resource_profile_combo_->setToolTip(Tr("tooltip_resource_profile"));
  }

  if (group_diagnostics_) group_diagnostics_->setTitle(Tr("group_diagnostics"));
  if (label_log_level_) label_log_level_->setText(Tr("label_log_level"));
  if (log_level_combo_) {
    log_level_combo_->setItemText(0, Tr("log_debug"));
    log_level_combo_->setItemText(1, Tr("log_info"));
    log_level_combo_->setItemText(2, Tr("log_warn"));
    log_level_combo_->setItemText(3, Tr("log_error"));
  }

  if (log_console_check_) log_console_check_->setText(Tr("chk_console_out"));
  if (log_sync_check_) log_sync_check_->setText(Tr("chk_sync_flush"));
  if (doctotext_log_check_) doctotext_log_check_->setText(Tr("chk_dt_log"));
  if (label_dt_path_) label_dt_path_->setText(Tr("label_dt_path"));
  if (doctotext_log_path_edit_) doctotext_log_path_edit_->setPlaceholderText(Tr("dt_log_ph"));
  if (dt_browse_btn_) dt_browse_btn_->setText(Tr("btn_browse"));
}

void PerformanceTab::OnBrowseDocToTextLogPath() {
  const QString file =
      QFileDialog::getSaveFileName(this, Tr("dlg_choose_log_file"), "", "Log Files (*.log);;All Files (*)");
  if (!file.isEmpty()) {
    doctotext_log_path_edit_->setText(file);
  }
}

}  // namespace gui
}  // namespace soeasy
