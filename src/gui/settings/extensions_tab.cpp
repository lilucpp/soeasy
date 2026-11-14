// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings/extensions_tab.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QVBoxLayout>

#include "common/format_categories.h"
#include "gui/i18n.h"
#include "service/index_config.h"

namespace soeasy {
namespace gui {

ExtensionsTab::ExtensionsTab(QWidget *parent) : ISettingsTab(parent) {
  SetupUi();
}

void ExtensionsTab::SetupUi() {
  auto *layout_exts = new QVBoxLayout(this);
  layout_exts->setContentsMargins(16, 16, 16, 16);
  layout_exts->setSpacing(10);

  // Group 1: 嵌入图片解析选项
  group_ocr_ = new QGroupBox(this);
  auto *layout_ocr = new QVBoxLayout(group_ocr_);
  embedded_image_ocr_check_ = new QCheckBox(group_ocr_);
  label_ocr_tip_ = new QLabel(group_ocr_);
  label_ocr_tip_->setStyleSheet("color: #666; font-size: 12px; margin-left: 20px;");
  layout_ocr->addWidget(embedded_image_ocr_check_);
  layout_ocr->addWidget(label_ocr_tip_);
  layout_exts->addWidget(group_ocr_);

  // Group 2: 文件分类树
  label_tree_title_ = new QLabel(this);
  layout_exts->addWidget(label_tree_title_);

  exts_tree_ = new QTreeWidget(this);
  exts_tree_->setColumnCount(2);
  exts_tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  exts_tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  exts_tree_->setAlternatingRowColors(true);
  layout_exts->addWidget(exts_tree_);

  // Populate preset categories into tree
  size_t cat_idx = 0;
  for (const auto &cat : common::GetPresetCategories()) {
    auto *cat_item = new QTreeWidgetItem(exts_tree_);
    cat_item->setData(0, Qt::UserRole + 1, static_cast<qulonglong>(cat_idx));
    cat_item->setText(0, I18n::Instance().GetCategoryName(cat_idx, QString::fromUtf8(cat.name)));
    cat_item->setText(1, I18n::Instance().GetCategoryDesc(cat_idx, QString::fromUtf8(cat.desc)));
    cat_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    QFont cat_font = cat_item->font(0);
    cat_font.setBold(true);
    cat_item->setFont(0, cat_font);

    for (const auto &fmt : cat.formats) {
      auto *fmt_item = new QTreeWidgetItem(cat_item);
      fmt_item->setText(0, QString::fromLatin1(fmt.ext));
      fmt_item->setData(0, Qt::UserRole, QString::fromLatin1(fmt.ext));
      fmt_item->setData(1, Qt::UserRole, QString::fromUtf8(fmt.desc));
      fmt_item->setData(1, Qt::UserRole + 1, (fmt.disabled || cat.disabled));
      QString desc = I18n::Instance().GetFormatDesc(QString::fromLatin1(fmt.ext), QString::fromUtf8(fmt.desc));
      if (fmt.disabled || cat.disabled) {
        desc += Tr("format_planned");
      }
      fmt_item->setText(1, desc);
      fmt_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
      fmt_item->setCheckState(0, fmt.default_enabled ? Qt::Checked : Qt::Unchecked);

      if (fmt.disabled || cat.disabled) {
        fmt_item->setDisabled(true);
        fmt_item->setCheckState(0, Qt::Unchecked);
        fmt_item->setToolTip(0, Tr("cat_disabled_tip"));
        fmt_item->setToolTip(1, Tr("cat_disabled_tip"));
      }
    }

    if (cat.disabled) {
      cat_item->setDisabled(true);
      cat_item->setCheckState(0, Qt::Unchecked);
      cat_item->setToolTip(0, Tr("cat_disabled_tip"));
      cat_item->setToolTip(1, Tr("cat_disabled_tip"));
    }
    ++cat_idx;
  }

  // Custom category item
  custom_category_item_ = new QTreeWidgetItem(exts_tree_);
  custom_category_item_->setText(0, "用户自定义格式 (Custom)");
  custom_category_item_->setText(1, "用户手动添加的专有文件后缀与提取规则");
  custom_category_item_->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
  QFont custom_font = custom_category_item_->font(0);
  custom_font.setBold(true);
  custom_category_item_->setFont(0, custom_font);

  exts_tree_->expandAll();

  // Custom extension management bar
  auto *add_custom_layout = new QHBoxLayout();
  label_custom_ = new QLabel(this);
  new_custom_ext_edit_ = new QLineEdit(this);
  btn_add_custom_ext_ = new QPushButton(this);
  btn_rm_custom_ext_ = new QPushButton(this);

  add_custom_layout->addWidget(label_custom_);
  add_custom_layout->addWidget(new_custom_ext_edit_);
  add_custom_layout->addWidget(btn_add_custom_ext_);
  add_custom_layout->addWidget(btn_rm_custom_ext_);
  layout_exts->addLayout(add_custom_layout);

  // Bottom action bar
  auto *bottom_exts_layout = new QHBoxLayout();
  btn_select_all_ = new QPushButton(this);
  btn_deselect_all_ = new QPushButton(this);
  btn_reset_exts_ = new QPushButton(this);
  exts_count_label_ = new QLabel(this);
  exts_count_label_->setStyleSheet("color: #1976D2; font-weight: 500;");

  bottom_exts_layout->addWidget(btn_select_all_);
  bottom_exts_layout->addWidget(btn_deselect_all_);
  bottom_exts_layout->addWidget(btn_reset_exts_);
  bottom_exts_layout->addStretch();
  bottom_exts_layout->addWidget(exts_count_label_);
  layout_exts->addLayout(bottom_exts_layout);

  connect(btn_add_custom_ext_, &QPushButton::clicked, this, &ExtensionsTab::OnAddCustomExtension);
  connect(new_custom_ext_edit_, &QLineEdit::returnPressed, this, &ExtensionsTab::OnAddCustomExtension);
  connect(btn_rm_custom_ext_, &QPushButton::clicked, this, &ExtensionsTab::OnRemoveCustomExtension);
  connect(btn_select_all_, &QPushButton::clicked, this, &ExtensionsTab::OnSelectAllExtensions);
  connect(btn_deselect_all_, &QPushButton::clicked, this, &ExtensionsTab::OnDeselectAllExtensions);
  connect(btn_reset_exts_, &QPushButton::clicked, this, &ExtensionsTab::OnResetExtensionsToDefault);
  connect(exts_tree_, &QTreeWidget::itemChanged, this, &ExtensionsTab::OnTreeItemChanged);

  RetranslateUi();
  UpdateExtensionsCount();
}

void ExtensionsTab::PopulateFromConfig(const service::IndexConfig &config) {
  is_updating_tree_ = true;

  embedded_image_ocr_check_->setChecked(config.enable_embedded_image_ocr());

  const auto &supported_exts = config.GetSupportedExtensions();

  // 1. Reset all preset formats to unchecked, then check if in supported_exts
  std::unordered_set<std::string> recognized_presets;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
    if (cat_item == custom_category_item_) continue;

    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto *fmt_item = cat_item->child(j);
      std::string ext = fmt_item->data(0, Qt::UserRole).toString().toStdString();
      recognized_presets.insert(ext);

      if (fmt_item->isDisabled()) {
        fmt_item->setCheckState(0, Qt::Unchecked);
      } else if (supported_exts.count(ext) > 0) {
        fmt_item->setCheckState(0, Qt::Checked);
      } else {
        fmt_item->setCheckState(0, Qt::Unchecked);
      }
    }
  }

  // 2. Clear and populate custom extensions
  while (custom_category_item_->childCount() > 0) {
    delete custom_category_item_->takeChild(0);
  }

  for (const auto &ext : supported_exts) {
    if (recognized_presets.find(ext) == recognized_presets.end()) {
      auto *custom_item = new QTreeWidgetItem(custom_category_item_);
      custom_item->setText(0, QString::fromStdString(ext));
      custom_item->setData(0, Qt::UserRole, QString::fromStdString(ext));
      custom_item->setText(1, Tr("custom_format_desc"));
      custom_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
      custom_item->setCheckState(0, Qt::Checked);
    }
  }

  is_updating_tree_ = false;
  UpdateExtensionsCount();
}

void ExtensionsTab::ApplyToConfig(service::IndexConfig *config) {
  if (!config || structural_read_only_) return;

  config->set_enable_embedded_image_ocr(embedded_image_ocr_check_->isChecked());

  std::vector<std::string> exts;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto *fmt_item = cat_item->child(j);
      if (fmt_item->checkState(0) == Qt::Checked) {
        exts.push_back(fmt_item->data(0, Qt::UserRole).toString().toStdString());
      }
    }
  }
  config->SetSupportedExtensions(exts);
}

void ExtensionsTab::SetStructuralReadOnly(bool read_only) {
  structural_read_only_ = read_only;
  if (group_ocr_) group_ocr_->setEnabled(!read_only);
  if (exts_tree_) exts_tree_->setEnabled(!read_only);
  if (new_custom_ext_edit_) new_custom_ext_edit_->setEnabled(!read_only);
  if (btn_add_custom_ext_) btn_add_custom_ext_->setEnabled(!read_only);
  if (btn_rm_custom_ext_) btn_rm_custom_ext_->setEnabled(!read_only);
  if (btn_select_all_) btn_select_all_->setEnabled(!read_only);
  if (btn_deselect_all_) btn_deselect_all_->setEnabled(!read_only);
  if (btn_reset_exts_) btn_reset_exts_->setEnabled(!read_only);
}

void ExtensionsTab::RetranslateUi() {
  if (group_ocr_) group_ocr_->setTitle(Tr("ocr_group_title"));
  if (embedded_image_ocr_check_) embedded_image_ocr_check_->setText(Tr("ocr_checkbox_label"));
  if (label_ocr_tip_) label_ocr_tip_->setText(Tr("ocr_tip_label"));
  if (label_tree_title_) label_tree_title_->setText(Tr("exts_tree_title"));
  if (exts_tree_) exts_tree_->setHeaderLabels({Tr("tree_col_category"), Tr("tree_col_desc")});

  if (label_custom_) label_custom_->setText(Tr("label_custom_ext"));
  if (new_custom_ext_edit_) new_custom_ext_edit_->setPlaceholderText(Tr("custom_ext_ph"));
  if (btn_add_custom_ext_) btn_add_custom_ext_->setText(Tr("btn_add_ext"));
  if (btn_rm_custom_ext_) btn_rm_custom_ext_->setText(Tr("btn_rm_custom_ext"));

  if (btn_select_all_) btn_select_all_->setText(Tr("btn_select_all"));
  if (btn_deselect_all_) btn_deselect_all_->setText(Tr("btn_deselect_all"));
  if (btn_reset_exts_) btn_reset_exts_->setText(Tr("btn_reset_exts"));

  if (custom_category_item_) {
    custom_category_item_->setText(0, Tr("cat_custom_title"));
    custom_category_item_->setText(1, Tr("cat_custom_desc"));
  }

  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
    if (cat_item == custom_category_item_) continue;
    size_t cat_idx = cat_item->data(0, Qt::UserRole + 1).toULongLong();
    cat_item->setText(0, I18n::Instance().GetCategoryName(cat_idx, cat_item->text(0)));
    cat_item->setText(1, I18n::Instance().GetCategoryDesc(cat_idx, cat_item->text(1)));

    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto *fmt_item = cat_item->child(j);
      QString ext = fmt_item->data(0, Qt::UserRole).toString();
      QString orig_desc = fmt_item->data(1, Qt::UserRole).toString();
      bool is_disabled = fmt_item->data(1, Qt::UserRole + 1).toBool();
      QString desc = I18n::Instance().GetFormatDesc(ext, orig_desc);
      if (is_disabled) {
        desc += Tr("format_planned");
      }
      fmt_item->setText(1, desc);
    }
  }

  UpdateExtensionsCount();
}

void ExtensionsTab::UpdateExtensionsCount() {
  int enabled_formats = 0;
  int total_formats = 0;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      total_formats++;
      if (cat_item->child(j)->checkState(0) == Qt::Checked) {
        enabled_formats++;
      }
    }
  }
  if (exts_count_label_) {
    exts_count_label_->setText(Tr("exts_count_status").arg(enabled_formats).arg(total_formats));
  }
}

void ExtensionsTab::OnTreeItemChanged(QTreeWidgetItem *, int) {
  if (is_updating_tree_) return;
  UpdateExtensionsCount();
}

void ExtensionsTab::OnSelectAllExtensions() {
  is_updating_tree_ = true;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
    if (cat_item->isDisabled()) continue;
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto *child = cat_item->child(j);
      if (!child->isDisabled()) {
        child->setCheckState(0, Qt::Checked);
      }
    }
  }
  is_updating_tree_ = false;
  UpdateExtensionsCount();
}

void ExtensionsTab::OnDeselectAllExtensions() {
  is_updating_tree_ = true;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      cat_item->child(j)->setCheckState(0, Qt::Unchecked);
    }
  }
  is_updating_tree_ = false;
  UpdateExtensionsCount();
}

void ExtensionsTab::OnResetExtensionsToDefault() {
  is_updating_tree_ = true;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
    if (cat_item == custom_category_item_) continue;
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto *child = cat_item->child(j);
      if (child->isDisabled()) {
        child->setCheckState(0, Qt::Unchecked);
      } else {
        child->setCheckState(0, Qt::Checked);
      }
    }
  }
  is_updating_tree_ = false;
  UpdateExtensionsCount();
}

void ExtensionsTab::OnAddCustomExtension() {
  QString ext = new_custom_ext_edit_->text().trimmed().toLower();
  if (ext.startsWith('.')) {
    ext = ext.mid(1);
  }
  if (ext.isEmpty()) return;

  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      if (cat_item->child(j)->data(0, Qt::UserRole).toString() == ext) {
        new_custom_ext_edit_->clear();
        return;
      }
    }
  }

  is_updating_tree_ = true;
  auto *custom_item = new QTreeWidgetItem(custom_category_item_);
  custom_item->setText(0, ext);
  custom_item->setData(0, Qt::UserRole, ext);
  custom_item->setText(1, Tr("custom_format_desc"));
  custom_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
  custom_item->setCheckState(0, Qt::Checked);
  is_updating_tree_ = false;

  new_custom_ext_edit_->clear();
  UpdateExtensionsCount();
}

void ExtensionsTab::OnRemoveCustomExtension() {
  auto *cur = exts_tree_->currentItem();
  if (!cur || cur->parent() != custom_category_item_) return;

  delete cur;
  UpdateExtensionsCount();
}

}  // namespace gui
}  // namespace soeasy
