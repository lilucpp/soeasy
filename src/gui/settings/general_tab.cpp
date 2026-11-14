// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/general_tab.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "gui/i18n.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

namespace {

bool ListContainsExact(const QListWidget *list, const QString &value) {
  for (int i = 0; i < list->count(); ++i) {
    if (list->item(i)->text() == value) {
      return true;
    }
  }
  return false;
}

void RemoveSelectedRow(QListWidget *list) {
  const int row = list->currentRow();
  if (row >= 0) {
    delete list->takeItem(row);
  }
}

}  // namespace

GeneralTab::GeneralTab(QWidget *parent) : ISettingsTab(parent) {
  SetupUi();
}

void GeneralTab::SetupUi() {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 14, 16, 14);
  layout->setSpacing(10);

  // Section 1: Appearance & Language
  group_appearance_ = new QGroupBox(this);
  auto *form_lang = new QFormLayout(group_appearance_);
  form_lang->setContentsMargins(12, 10, 12, 10);
  form_lang->setSpacing(6);

  label_language_ = new QLabel(group_appearance_);
  language_combo_ = new QComboBox(group_appearance_);
  language_combo_->addItem("简体中文 (Simplified Chinese)", "zh");
  language_combo_->addItem("English", "en");
  form_lang->addRow(label_language_, language_combo_);

  label_lang_hint_ = new QLabel(group_appearance_);
  label_lang_hint_->setStyleSheet("color: #64748B; font-size: 12px;");
  form_lang->addRow("", label_lang_hint_);
  layout->addWidget(group_appearance_);

  // Section 2: Indexed Directories
  group_dirs_ = new QGroupBox(this);
  auto *dirs_layout = new QVBoxLayout(group_dirs_);
  dirs_layout->setContentsMargins(12, 10, 12, 10);
  dirs_layout->setSpacing(8);

  label_dirs_ = new QLabel(group_dirs_);
  dirs_list_ = new QListWidget(group_dirs_);
  dirs_list_->setMinimumHeight(88);

  auto *dir_btn_layout = new QHBoxLayout();
  btn_add_dir_ = new QPushButton(group_dirs_);
  btn_rm_dir_ = new QPushButton(group_dirs_);
  dir_btn_layout->addWidget(btn_add_dir_);
  dir_btn_layout->addWidget(btn_rm_dir_);
  dir_btn_layout->addStretch();

  dirs_layout->addWidget(label_dirs_);
  dirs_layout->addWidget(dirs_list_, 1);
  dirs_layout->addLayout(dir_btn_layout);
  layout->addWidget(group_dirs_, 1);

  // Section 3: Exclusions & Filters
  group_exclusions_ = new QGroupBox(this);
  auto *excl_layout = new QVBoxLayout(group_exclusions_);
  excl_layout->setContentsMargins(12, 10, 12, 10);
  excl_layout->setSpacing(8);

  label_patterns_ = new QLabel(group_exclusions_);
  patterns_list_ = new QListWidget(group_exclusions_);
  patterns_list_->setMinimumHeight(88);

  auto *add_pattern_layout = new QHBoxLayout();
  new_pattern_edit_ = new QLineEdit(group_exclusions_);
  btn_add_pattern_ = new QPushButton(group_exclusions_);
  btn_rm_pattern_ = new QPushButton(group_exclusions_);
  add_pattern_layout->addWidget(new_pattern_edit_);
  add_pattern_layout->addWidget(btn_add_pattern_);
  add_pattern_layout->addWidget(btn_rm_pattern_);

  filter_thumbs_check_ = new QCheckBox(group_exclusions_);

  excl_layout->addWidget(label_patterns_);
  excl_layout->addWidget(patterns_list_, 1);
  excl_layout->addLayout(add_pattern_layout);
  excl_layout->addWidget(filter_thumbs_check_);
  layout->addWidget(group_exclusions_, 1);

  connect(language_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    const std::string chosen_lang = language_combo_->currentData().toString().toStdString();
    I18n::Instance().SetLanguage(chosen_lang);
    emit LanguageChanged(chosen_lang);
  });
  connect(btn_add_dir_, &QPushButton::clicked, this, &GeneralTab::OnAddDirectory);
  connect(btn_rm_dir_, &QPushButton::clicked, this, &GeneralTab::OnRemoveDirectory);
  connect(btn_add_pattern_, &QPushButton::clicked, this, &GeneralTab::OnAddPattern);
  connect(new_pattern_edit_, &QLineEdit::returnPressed, this, &GeneralTab::OnAddPattern);
  connect(btn_rm_pattern_, &QPushButton::clicked, this, &GeneralTab::OnRemovePattern);

  RetranslateUi();
}

void GeneralTab::PopulateFromConfig(const service::IndexConfig &config) {
  const QString lang = QString::fromStdString(config.ui_language());
  const int idx = language_combo_->findData(lang);
  if (idx >= 0) {
    QSignalBlocker blocker(language_combo_);
    language_combo_->setCurrentIndex(idx);
  }

  dirs_list_->clear();
  for (const auto &d : config.GetIndexedDirectories()) {
    dirs_list_->addItem(QString::fromStdString(d));
  }

  patterns_list_->clear();
  for (const auto &p : config.GetExcludedPatterns()) {
    patterns_list_->addItem(QString::fromStdString(p));
  }

  filter_thumbs_check_->setChecked(config.filter_cache_thumbnails());
}

void GeneralTab::ApplyToConfig(service::IndexConfig *config) {
  if (!config) return;
  config->set_ui_language(language_combo_->currentData().toString().toStdString());

  if (structural_read_only_) {
    return;
  }

  std::vector<std::string> dirs;
  dirs.reserve(static_cast<size_t>(dirs_list_->count()));
  for (int i = 0; i < dirs_list_->count(); ++i) {
    dirs.push_back(dirs_list_->item(i)->text().toStdString());
  }
  config->SetIndexedDirectories(dirs);

  std::vector<std::string> pats;
  pats.reserve(static_cast<size_t>(patterns_list_->count()));
  for (int i = 0; i < patterns_list_->count(); ++i) {
    pats.push_back(patterns_list_->item(i)->text().toStdString());
  }
  config->SetExcludedPatterns(pats);

  config->set_filter_cache_thumbnails(filter_thumbs_check_->isChecked());
}

void GeneralTab::SetStructuralReadOnly(bool read_only) {
  structural_read_only_ = read_only;
  if (group_dirs_) group_dirs_->setEnabled(!read_only);
  if (group_exclusions_) group_exclusions_->setEnabled(!read_only);
}

void GeneralTab::RetranslateUi() {
  if (group_appearance_) group_appearance_->setTitle(Tr("group_appearance"));
  if (label_language_) label_language_->setText(Tr("label_ui_language"));
  if (label_lang_hint_) label_lang_hint_->setText(Tr("lang_hint"));

  if (group_dirs_) group_dirs_->setTitle(Tr("group_indexed_dirs"));
  if (label_dirs_) label_dirs_->setText(Tr("label_dirs"));
  if (btn_add_dir_) btn_add_dir_->setText(Tr("btn_add_dir"));
  if (btn_rm_dir_) btn_rm_dir_->setText(Tr("btn_rm_dir"));

  if (group_exclusions_) group_exclusions_->setTitle(Tr("group_exclusions"));
  if (label_patterns_) label_patterns_->setText(Tr("label_patterns"));
  if (new_pattern_edit_) new_pattern_edit_->setPlaceholderText(Tr("pattern_ph"));
  if (btn_add_pattern_) btn_add_pattern_->setText(Tr("btn_add_pattern"));
  if (btn_rm_pattern_) btn_rm_pattern_->setText(Tr("btn_rm_dir"));
  if (filter_thumbs_check_) filter_thumbs_check_->setText(Tr("filter_thumbs_check"));
}

void GeneralTab::OnAddDirectory() {
  const QString dir = QFileDialog::getExistingDirectory(this, Tr("dlg_choose_dir"));
  if (!dir.isEmpty() && !ListContainsExact(dirs_list_, dir)) {
    dirs_list_->addItem(dir);
  }
}

void GeneralTab::OnRemoveDirectory() {
  RemoveSelectedRow(dirs_list_);
}

void GeneralTab::OnAddPattern() {
  const QString pat = new_pattern_edit_->text().trimmed();
  if (pat.isEmpty()) return;
  if (!ListContainsExact(patterns_list_, pat)) {
    patterns_list_->addItem(pat);
  }
  new_pattern_edit_->clear();
}

void GeneralTab::OnRemovePattern() {
  RemoveSelectedRow(patterns_list_);
}

}  // namespace gui
}  // namespace soeasy
