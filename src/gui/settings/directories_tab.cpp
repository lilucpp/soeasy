// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/directories_tab.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "gui/i18n.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

DirectoriesTab::DirectoriesTab(QWidget *parent) : ISettingsTab(parent) {
  SetupUi();
}

void DirectoriesTab::SetupUi() {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(12);

  label_dirs_ = new QLabel(this);
  dirs_list_ = new QListWidget(this);

  auto *btn_layout = new QHBoxLayout();
  btn_add_dir_ = new QPushButton(this);
  btn_rm_dir_ = new QPushButton(this);
  btn_layout->addWidget(btn_add_dir_);
  btn_layout->addWidget(btn_rm_dir_);
  btn_layout->addStretch();

  layout->addWidget(label_dirs_);
  layout->addWidget(dirs_list_);
  layout->addLayout(btn_layout);

  connect(btn_add_dir_, &QPushButton::clicked, this, &DirectoriesTab::OnAddDirectory);
  connect(btn_rm_dir_, &QPushButton::clicked, this, &DirectoriesTab::OnRemoveDirectory);

  RetranslateUi();
}

void DirectoriesTab::PopulateFromConfig(const service::IndexConfig &config) {
  dirs_list_->clear();
  for (const auto &d : config.GetIndexedDirectories()) {
    dirs_list_->addItem(QString::fromStdString(d));
  }
}

void DirectoriesTab::ApplyToConfig(service::IndexConfig *config) {
  if (!config) return;
  std::vector<std::string> dirs;
  for (int i = 0; i < dirs_list_->count(); ++i) {
    dirs.push_back(dirs_list_->item(i)->text().toStdString());
  }
  config->SetIndexedDirectories(dirs);
}

void DirectoriesTab::RetranslateUi() {
  if (label_dirs_) label_dirs_->setText(Tr("label_dirs"));
  if (btn_add_dir_) btn_add_dir_->setText(Tr("btn_add_dir"));
  if (btn_rm_dir_) btn_rm_dir_->setText(Tr("btn_rm_dir"));
}

void DirectoriesTab::OnAddDirectory() {
  QString dir = QFileDialog::getExistingDirectory(this, Tr("dlg_choose_dir"));
  if (!dir.isEmpty()) {
    for (int i = 0; i < dirs_list_->count(); ++i) {
      if (dirs_list_->item(i)->text() == dir) {
        return;
      }
    }
    dirs_list_->addItem(dir);
  }
}

void DirectoriesTab::OnRemoveDirectory() {
  int row = dirs_list_->currentRow();
  if (row >= 0) {
    delete dirs_list_->takeItem(row);
  }
}

}  // namespace gui
}  // namespace soeasy
