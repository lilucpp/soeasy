// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/user_dict_tab.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <vector>

#include "gui/i18n.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

UserDictTab::UserDictTab(QWidget *parent) : ISettingsTab(parent) {
  SetupUi();
}

void UserDictTab::SetupUi() {
  auto *layout_dict = new QVBoxLayout(this);
  layout_dict->setContentsMargins(16, 16, 16, 16);
  layout_dict->setSpacing(12);

  label_user_dict_hint_ = new QLabel(this);
  label_user_dict_hint_->setWordWrap(true);
  layout_dict->addWidget(label_user_dict_hint_);

  user_dict_table_ = new QTableWidget(this);
  user_dict_table_->setColumnCount(1);
  user_dict_table_->horizontalHeader()->setStretchLastSection(true);
  user_dict_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  user_dict_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  user_dict_table_->setAlternatingRowColors(true);
  layout_dict->addWidget(user_dict_table_);

  auto *input_layout = new QHBoxLayout();
  user_word_input_ = new QLineEdit(this);
  btn_add_user_word_ = new QPushButton(this);
  btn_rm_user_word_ = new QPushButton(this);

  input_layout->addWidget(user_word_input_, 1);
  input_layout->addWidget(btn_add_user_word_);
  input_layout->addWidget(btn_rm_user_word_);
  layout_dict->addLayout(input_layout);

  auto *bottom_layout = new QHBoxLayout();
  user_dict_count_label_ = new QLabel(this);
  user_dict_count_label_->setStyleSheet("color: #64748B; font-size: 12px;");
  bottom_layout->addStretch();
  bottom_layout->addWidget(user_dict_count_label_);
  layout_dict->addLayout(bottom_layout);

  connect(btn_add_user_word_, &QPushButton::clicked, this, &UserDictTab::OnAddUserWord);
  connect(user_word_input_, &QLineEdit::returnPressed, this, &UserDictTab::OnAddUserWord);
  connect(btn_rm_user_word_, &QPushButton::clicked, this, &UserDictTab::OnRemoveUserWord);

  RetranslateUi();
}

void UserDictTab::PopulateFromConfig(const service::IndexConfig &config) {
  QString dict_dir = !config.dict_dir().empty() ? QString::fromStdString(config.dict_dir())
                                                : QCoreApplication::applicationDirPath() + "/dict";
  QString appdata_dict_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/dict";

  if (!dictionary_service_) {
    dictionary_service_ = std::make_unique<service::DictionaryService>();
    dictionary_service_->InitBaseDictionary({dict_dir.toStdString(), appdata_dict_dir.toStdString()});
  }

  user_dict_file_path_.clear();
  QString appdata_path = appdata_dict_dir + "/user.dict.utf8";
  if (!config.dict_dir().empty()) {
    QString cfg_dict = QString::fromStdString(config.dict_dir()) + "/user.dict.utf8";
    if (QFileInfo::exists(cfg_dict)) {
      user_dict_file_path_ = cfg_dict;
    }
  }
  if (user_dict_file_path_.isEmpty()) {
    if (QFileInfo::exists(appdata_path)) {
      user_dict_file_path_ = appdata_path;
    } else {
      QString appdir_path = QCoreApplication::applicationDirPath() + "/dict/user.dict.utf8";
      user_dict_file_path_ = QFileInfo::exists(appdir_path) ? appdir_path : appdata_path;
    }
  }

  LoadUserDict();
}

void UserDictTab::ApplyToConfig(service::IndexConfig *) {
  SaveUserDict();
}

void UserDictTab::RetranslateUi() {
  if (label_user_dict_hint_) label_user_dict_hint_->setText(Tr("label_user_dict_hint"));
  if (user_dict_table_) user_dict_table_->setHorizontalHeaderLabels({Tr("col_user_dict_word")});
  if (user_word_input_) user_word_input_->setPlaceholderText(Tr("word_ph"));
  if (btn_add_user_word_) btn_add_user_word_->setText(Tr("btn_add_word"));
  if (btn_rm_user_word_) btn_rm_user_word_->setText(Tr("btn_rm_dir"));

  if (user_dict_count_label_ && user_dict_table_) {
    user_dict_count_label_->setText(Tr("label_word_count").arg(user_dict_table_->rowCount()));
  }
}

void UserDictTab::LoadUserDict() {
  user_dict_table_->setRowCount(0);
  if (user_dict_file_path_.isEmpty() || !dictionary_service_ || !QFileInfo::exists(user_dict_file_path_)) {
    if (user_dict_count_label_) user_dict_count_label_->setText(Tr("label_word_count").arg(0));
    return;
  }

  const auto words = dictionary_service_->LoadUserWords(user_dict_file_path_.toStdString());
  int row = 0;
  for (const auto &word : words) {
    user_dict_table_->insertRow(row);
    auto *item = new QTableWidgetItem(QString::fromStdString(word));
    user_dict_table_->setItem(row, 0, item);
    row++;
  }

  if (user_dict_count_label_) {
    user_dict_count_label_->setText(Tr("label_word_count").arg(user_dict_table_->rowCount()));
  }
}

void UserDictTab::SaveUserDict() {
  if (!dictionary_service_) {
    return;
  }
  if (user_dict_file_path_.isEmpty()) {
    user_dict_file_path_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/dict/user.dict.utf8";
  }

  std::vector<std::string> words;
  words.reserve(static_cast<size_t>(user_dict_table_->rowCount()));
  for (int r = 0; r < user_dict_table_->rowCount(); ++r) {
    auto *item = user_dict_table_->item(r, 0);
    if (item && !item->text().trimmed().isEmpty()) {
      words.push_back(item->text().trimmed().toStdString());
    }
  }
  dictionary_service_->SaveUserWords(user_dict_file_path_.toStdString(), words);
}

void UserDictTab::OnAddUserWord() {
  QString word = user_word_input_->text().trimmed();
  if (word.isEmpty()) return;

  // Check duplicate in table
  for (int r = 0; r < user_dict_table_->rowCount(); ++r) {
    auto *item = user_dict_table_->item(r, 0);
    if (item && item->text() == word) {
      QMessageBox::information(this, Tr("dlg_tip"), Tr("word_exists_dict").arg(word));
      user_word_input_->clear();
      return;
    }
  }

  // Check existence in Jieba DAT base dictionary via Service facade
  if (dictionary_service_ && dictionary_service_->IsInBaseDictionary(word.toStdString())) {
    QMessageBox::information(this, Tr("dlg_tip"), Tr("word_exists_base").arg(word));
    user_word_input_->clear();
    return;
  }

  int row = user_dict_table_->rowCount();
  user_dict_table_->insertRow(row);
  auto *item = new QTableWidgetItem(word);
  user_dict_table_->setItem(row, 0, item);

  user_word_input_->clear();
  if (user_dict_count_label_) {
    user_dict_count_label_->setText(Tr("label_word_count").arg(user_dict_table_->rowCount()));
  }
}

void UserDictTab::OnRemoveUserWord() {
  auto selected = user_dict_table_->selectionModel()->selectedRows();
  if (selected.isEmpty()) return;

  std::vector<int> rows;
  rows.reserve(selected.size());
  for (const auto &idx : selected) {
    rows.push_back(idx.row());
  }
  std::sort(rows.rbegin(), rows.rend());

  for (int r : rows) {
    user_dict_table_->removeRow(r);
  }

  if (user_dict_count_label_) {
    user_dict_count_label_->setText(Tr("label_word_count").arg(user_dict_table_->rowCount()));
  }
}

}  // namespace gui
}  // namespace soeasy
