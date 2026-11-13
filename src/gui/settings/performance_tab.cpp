// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/performance_tab.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>

#include "gui/i18n.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

PerformanceTab::PerformanceTab(QWidget *parent) : ISettingsTab(parent) {
  SetupUi();
}

void PerformanceTab::SetupUi() {
  auto *form_perf = new QFormLayout(this);
  form_perf->setContentsMargins(16, 16, 16, 16);
  form_perf->setSpacing(12);

  debounce_spin_ = new QSpinBox(this);
  debounce_spin_->setRange(50, 5000);
  debounce_spin_->setSingleStep(50);
  label_debounce_ = new QLabel(this);
  form_perf->addRow(label_debounce_, debounce_spin_);

  max_size_spin_ = new QSpinBox(this);
  max_size_spin_->setRange(1, 1024);
  max_size_spin_->setSingleStep(10);
  label_max_size_ = new QLabel(this);
  form_perf->addRow(label_max_size_, max_size_spin_);

  workers_spin_ = new QSpinBox(this);
  workers_spin_->setRange(1, 16);
  workers_spin_->setSingleStep(1);
  label_workers_ = new QLabel(this);
  form_perf->addRow(label_workers_, workers_spin_);

  filter_thumbs_check_ = new QCheckBox(this);
  form_perf->addRow("", filter_thumbs_check_);

  log_level_combo_ = new QComboBox(this);
  log_level_combo_->addItem("DEBUG", "debug");
  log_level_combo_->addItem("INFO", "info");
  log_level_combo_->addItem("WARN", "warn");
  log_level_combo_->addItem("ERROR", "error");
  label_log_level_ = new QLabel(this);
  form_perf->addRow(label_log_level_, log_level_combo_);

  log_console_check_ = new QCheckBox(this);
  form_perf->addRow(Tr("label_console_out"), log_console_check_);

  log_sync_check_ = new QCheckBox(this);
  form_perf->addRow(Tr("label_sync_flush"), log_sync_check_);

  doctotext_log_check_ = new QCheckBox(this);
  form_perf->addRow(Tr("label_dt_log"), doctotext_log_check_);

  auto *dt_path_layout = new QHBoxLayout();
  doctotext_log_path_edit_ = new QLineEdit(this);
  dt_browse_btn_ = new QPushButton(this);
  dt_browse_btn_->setFixedWidth(70);
  dt_path_layout->addWidget(doctotext_log_path_edit_);
  dt_path_layout->addWidget(dt_browse_btn_);
  label_dt_path_ = new QLabel(this);
  form_perf->addRow(label_dt_path_, dt_path_layout);

  connect(dt_browse_btn_, &QPushButton::clicked, this, &PerformanceTab::OnBrowseDocToTextLogPath);

  RetranslateUi();
}

void PerformanceTab::PopulateFromConfig(const service::IndexConfig &config) {
  debounce_spin_->setValue(static_cast<int>(config.debounce_ms()));
  max_size_spin_->setValue(static_cast<int>(config.max_file_size() / (1024 * 1024)));
  workers_spin_->setValue(static_cast<int>(config.worker_count()));
  filter_thumbs_check_->setChecked(config.filter_cache_thumbnails());

  int idx = log_level_combo_->findData(QString::fromStdString(config.log_level()));
  if (idx >= 0) {
    log_level_combo_->setCurrentIndex(idx);
  } else {
    log_level_combo_->setCurrentIndex(1);  // default INFO
  }
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
  config->set_filter_cache_thumbnails(filter_thumbs_check_->isChecked());

  config->set_log_level(log_level_combo_->currentData().toString().toStdString());
  config->set_log_console(log_console_check_->isChecked());
  config->set_log_sync(log_sync_check_->isChecked());
  config->set_doctotext_logging(doctotext_log_check_->isChecked());
  config->set_doctotext_log_path(doctotext_log_path_edit_->text().trimmed().toStdString());
}

void PerformanceTab::RetranslateUi() {
  if (label_debounce_) label_debounce_->setText(Tr("label_debounce"));
  if (label_max_size_) label_max_size_->setText(Tr("label_max_size"));
  if (label_workers_) label_workers_->setText(Tr("label_workers"));
  if (workers_spin_) workers_spin_->setSuffix(Tr("suffix_workers"));
  if (filter_thumbs_check_) filter_thumbs_check_->setText(Tr("filter_thumbs_check"));

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
  QString file = QFileDialog::getSaveFileName(this, Tr("dlg_choose_log_file"), "", "Log Files (*.log);;All Files (*)");
  if (!file.isEmpty()) {
    doctotext_log_path_edit_->setText(file);
  }
}

}  // namespace gui
}  // namespace soeasy
