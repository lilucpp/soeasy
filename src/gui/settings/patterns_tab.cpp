// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/patterns_tab.h"

#include <QHBoxLayout>
#include <QVBoxLayout>

#include "gui/i18n.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

PatternsTab::PatternsTab(QWidget *parent) : ISettingsTab(parent) {
  SetupUi();
}

void PatternsTab::SetupUi() {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(12);

  label_patterns_ = new QLabel(this);
  patterns_list_ = new QListWidget(this);

  auto *add_pattern_layout = new QHBoxLayout();
  new_pattern_edit_ = new QLineEdit(this);
  btn_add_pattern_ = new QPushButton(this);
  btn_rm_pattern_ = new QPushButton(this);
  add_pattern_layout->addWidget(new_pattern_edit_);
  add_pattern_layout->addWidget(btn_add_pattern_);
  add_pattern_layout->addWidget(btn_rm_pattern_);

  layout->addWidget(label_patterns_);
  layout->addWidget(patterns_list_);
  layout->addLayout(add_pattern_layout);

  connect(btn_add_pattern_, &QPushButton::clicked, this, &PatternsTab::OnAddPattern);
  connect(new_pattern_edit_, &QLineEdit::returnPressed, this, &PatternsTab::OnAddPattern);
  connect(btn_rm_pattern_, &QPushButton::clicked, this, &PatternsTab::OnRemovePattern);

  RetranslateUi();
}

void PatternsTab::PopulateFromConfig(const service::IndexConfig &config) {
  patterns_list_->clear();
  for (const auto &p : config.GetExcludedPatterns()) {
    patterns_list_->addItem(QString::fromStdString(p));
  }
}

void PatternsTab::ApplyToConfig(service::IndexConfig *config) {
  if (!config) return;
  std::vector<std::string> pats;
  for (int i = 0; i < patterns_list_->count(); ++i) {
    pats.push_back(patterns_list_->item(i)->text().toStdString());
  }
  config->SetExcludedPatterns(pats);
}

void PatternsTab::RetranslateUi() {
  if (label_patterns_) label_patterns_->setText(Tr("label_patterns"));
  if (new_pattern_edit_) new_pattern_edit_->setPlaceholderText(Tr("pattern_ph"));
  if (btn_add_pattern_) btn_add_pattern_->setText(Tr("btn_add_pattern"));
  if (btn_rm_pattern_) btn_rm_pattern_->setText(Tr("btn_rm_dir"));
}

void PatternsTab::OnAddPattern() {
  QString pat = new_pattern_edit_->text().trimmed();
  if (!pat.isEmpty()) {
    for (int i = 0; i < patterns_list_->count(); ++i) {
      if (patterns_list_->item(i)->text() == pat) {
        new_pattern_edit_->clear();
        return;
      }
    }
    patterns_list_->addItem(pat);
    new_pattern_edit_->clear();
  }
}

void PatternsTab::OnRemovePattern() {
  int row = patterns_list_->currentRow();
  if (row >= 0) {
    delete patterns_list_->takeItem(row);
  }
}

}  // namespace gui
}  // namespace soeasy
