// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/user_dict_tab.h"

#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QVBoxLayout>

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
  QString dict_dir;
  if (!config.dict_dir().empty()) {
    dict_dir = QString::fromStdString(config.dict_dir());
  } else {
    dict_dir = QCoreApplication::applicationDirPath() + "/dict";
  }

  if (!jieba_) {
    jieba_ = std::make_unique<storage::DatJieba>();
    if (!jieba_->Init(dict_dir.toStdString())) {
      QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/dict";
      jieba_->Init(fallback.toStdString());
    }
  }

  user_dict_file_path_.clear();
  QString appdata_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/dict/user.dict.utf8";
  if (!config.dict_dir().empty()) {
    QString cfg_dict = QString::fromStdString(config.dict_dir()) + "/user.dict.utf8";
    if (QFile::exists(cfg_dict)) {
      user_dict_file_path_ = cfg_dict;
    }
  }
  if (user_dict_file_path_.isEmpty()) {
    if (QFile::exists(appdata_path)) {
      user_dict_file_path_ = appdata_path;
    } else {
      QString appdir_path = QCoreApplication::applicationDirPath() + "/dict/user.dict.utf8";
      if (QFile::exists(appdir_path)) {
        user_dict_file_path_ = appdir_path;
      } else {
        user_dict_file_path_ = appdata_path;
      }
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
  if (user_dict_file_path_.isEmpty() || !QFile::exists(user_dict_file_path_)) {
    if (user_dict_count_label_) user_dict_count_label_->setText(Tr("label_word_count").arg(0));
    return;
  }

  QFile file(user_dict_file_path_);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return;
  }

  QTextStream in(&file);
  in.setCodec("UTF-8");
  int row = 0;
  while (!in.atEnd()) {
    QString line = in.readLine().trimmed();
    if (line.isEmpty() || line.startsWith("#")) continue;

    QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.isEmpty()) continue;
    QString word = parts[0];

    user_dict_table_->insertRow(row);
    auto *item = new QTableWidgetItem(word);
    user_dict_table_->setItem(row, 0, item);
    row++;
  }

  if (user_dict_count_label_) {
    user_dict_count_label_->setText(Tr("label_word_count").arg(user_dict_table_->rowCount()));
  }
}

void UserDictTab::SaveUserDict() {
  if (user_dict_file_path_.isEmpty()) {
    user_dict_file_path_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/dict/user.dict.utf8";
  }

  QFileInfo fi(user_dict_file_path_);
  QDir().mkpath(fi.absolutePath());

  QFile file(user_dict_file_path_);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    return;
  }

  QTextStream out(&file);
  out.setCodec("UTF-8");
  for (int r = 0; r < user_dict_table_->rowCount(); ++r) {
    auto *item = user_dict_table_->item(r, 0);
    if (item && !item->text().trimmed().isEmpty()) {
      out << item->text().trimmed() << "\n";
    }
  }
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

  // Check existence in Jieba DAT base dictionary
  if (jieba_) {
    std::string word_std = word.toStdString();
    if (jieba_->HasWord(word_std)) {
      QMessageBox::information(this, Tr("dlg_tip"), Tr("word_exists_base").arg(word));
      user_word_input_->clear();
      return;
    }
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
