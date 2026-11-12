// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/general_tab.h"

#include <QFormLayout>

#include "gui/i18n.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

GeneralTab::GeneralTab(QWidget *parent) : ISettingsTab(parent) {
  SetupUi();
}

void GeneralTab::SetupUi() {
  auto *form = new QFormLayout(this);
  form->setContentsMargins(16, 16, 16, 16);
  form->setSpacing(12);

  label_language_ = new QLabel("界面显示语言 (UI Language):", this);
  language_combo_ = new QComboBox(this);
  language_combo_->addItem("简体中文 (Simplified Chinese)", "zh");
  language_combo_->addItem("English", "en");
  form->addRow(label_language_, language_combo_);

  label_lang_hint_ = new QLabel(this);
  label_lang_hint_->setStyleSheet("color: #64748B; font-size: 12px; margin-top: 4px;");
  form->addRow("", label_lang_hint_);

  connect(language_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    std::string chosen_lang = language_combo_->currentData().toString().toStdString();
    I18n::Instance().SetLanguage(chosen_lang);
    emit LanguageChanged(chosen_lang);
  });

  RetranslateUi();
}

void GeneralTab::PopulateFromConfig(const service::IndexConfig &config) {
  QString lang = QString::fromStdString(config.ui_language());
  int idx = language_combo_->findData(lang);
  if (idx >= 0) {
    QSignalBlocker blocker(language_combo_);
    language_combo_->setCurrentIndex(idx);
  }
}

void GeneralTab::ApplyToConfig(service::IndexConfig *config) {
  if (!config) return;
  config->set_ui_language(language_combo_->currentData().toString().toStdString());
}

void GeneralTab::RetranslateUi() {
  if (label_language_) label_language_->setText(Tr("label_ui_language"));
  if (label_lang_hint_) label_lang_hint_->setText(Tr("lang_hint"));
}

}  // namespace gui
}  // namespace soeasy
