// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/settings_dialog.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "gui/i18n.h"
#include "gui/settings/directories_tab.h"
#include "gui/settings/extensions_tab.h"
#include "gui/settings/general_tab.h"
#include "gui/settings/patterns_tab.h"
#include "gui/settings/performance_tab.h"

namespace soeasy {
namespace gui {

SettingsDialog::SettingsDialog(std::shared_ptr<service::IndexConfig> config, QWidget *parent)
    : QDialog(parent), config_(std::move(config)) {
  SetupUi();
  PopulateFromConfig();
}

void SettingsDialog::SetupUi() {
  resize(860, 600);
  setStyleSheet(
      "QDialog {"
      "  background-color: #FFFFFF;"
      "  color: #1E293B;"
      "  font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI', 'PingFang SC', 'Noto Sans CJK SC', "
      "'WenQuanYi Micro Hei', sans-serif;"
      "}"
      "QListWidget#settings_nav_list {"
      "  background-color: #F8FAFC;"
      "  border: none;"
      "  border-right: 1px solid #E2E8F0;"
      "  outline: none;"
      "  padding: 12px 8px;"
      "}"
      "QListWidget#settings_nav_list::item {"
      "  min-height: 40px;"
      "  max-height: 44px;"
      "  border-radius: 6px;"
      "  color: #334155;"
      "  font-size: 13px;"
      "  font-weight: 500;"
      "  padding-left: 14px;"
      "  margin-bottom: 6px;"
      "}"
      "QListWidget#settings_nav_list::item:hover {"
      "  background-color: #F1F5F9;"
      "  color: #0F172A;"
      "}"
      "QListWidget#settings_nav_list::item:selected {"
      "  background-color: #E0F2FE;"
      "  color: #0284C7;"
      "  font-weight: 600;"
      "}"
      "QStackedWidget#settings_page_stack {"
      "  background-color: #FFFFFF;"
      "  border: none;"
      "}"
      "QWidget#settings_bottom_container {"
      "  background-color: #F8FAFC;"
      "  border-top: 1px solid #E2E8F0;"
      "  padding: 8px 16px;"
      "}"
      "QTreeWidget, QListWidget, QTableWidget {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 6px;"
      "  color: #1E293B;"
      "}"
      "QLineEdit {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 8px;"
      "  color: #1E293B;"
      "  min-height: 24px;"
      "}"
      "QLineEdit:hover { border-color: #94A3B8; }"
      "QLineEdit:focus { border-color: #0284C7; }"
      "QComboBox {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 28px 4px 10px;"
      "  color: #1E293B;"
      "  min-height: 24px;"
      "}"
      "QComboBox:hover { border-color: #94A3B8; }"
      "QComboBox:focus, QComboBox:on { border-color: #0284C7; }"
      "QComboBox::drop-down {"
      "  subcontrol-origin: padding;"
      "  subcontrol-position: top right;"
      "  width: 26px;"
      "  border: none;"
      "  background: transparent;"
      "}"
      "QComboBox::down-arrow {"
      "  image: url(:/icons/chevron_down.png);"
      "  width: 12px;"
      "  height: 12px;"
      "}"
      "QComboBox QAbstractItemView {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  background-color: #FFFFFF;"
      "  selection-background-color: #E0F2FE;"
      "  selection-color: #0284C7;"
      "  padding: 4px;"
      "  outline: none;"
      "}"
      "QSpinBox {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 24px 4px 10px;"
      "  color: #1E293B;"
      "  min-height: 24px;"
      "}"
      "QSpinBox:hover { border-color: #94A3B8; }"
      "QSpinBox:focus { border-color: #0284C7; }"
      "QSpinBox::up-button, QSpinBox::down-button {"
      "  width: 20px;"
      "  border: none;"
      "  background: #F1F5F9;"
      "}"
      "QSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: 5px; }"
      "QSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 5px; }"
      "QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: #E2E8F0; }"
      "QSpinBox::up-arrow {"
      "  image: url(:/icons/chevron_up.png);"
      "  width: 10px;"
      "  height: 10px;"
      "}"
      "QSpinBox::down-arrow {"
      "  image: url(:/icons/chevron_down.png);"
      "  width: 10px;"
      "  height: 10px;"
      "}"
      "QHeaderView::section {"
      "  background-color: #F8FAFC;"
      "  color: #475569;"
      "  font-weight: 600;"
      "  font-size: 12px;"
      "  border: none;"
      "  border-bottom: 1px solid #E2E8F0;"
      "  padding: 5px 8px;"
      "}"
      "QPushButton {"
      "  background-color: #F8FAFC;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  padding: 6px 14px;"
      "  color: #334155;"
      "}"
      "QPushButton:hover { background-color: #F1F5F9; border-color: #94A3B8; color: #0F172A; }"
      "QPushButton:pressed { background-color: #E2E8F0; }"
      "QPushButton:default {"
      "  background-color: #0284C7;"
      "  border-color: #0369A1;"
      "  color: #FFFFFF;"
      "}"
      "QPushButton:default:hover { background-color: #0369A1; }");

  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  auto *body_widget = new QWidget(this);
  auto *body_layout = new QHBoxLayout(body_widget);
  body_layout->setContentsMargins(0, 0, 0, 0);
  body_layout->setSpacing(0);

  nav_list_ = new QListWidget(body_widget);
  nav_list_->setObjectName("settings_nav_list");
  nav_list_->setFixedWidth(175);
  nav_list_->setFocusPolicy(Qt::NoFocus);
  nav_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_list_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  page_stack_ = new QStackedWidget(body_widget);
  page_stack_->setObjectName("settings_page_stack");

  nav_title_keys_.clear();
  tabs_.clear();
  auto register_tab = [this](const char *title_key, ISettingsTab *tab) {
    auto *item = new QListWidgetItem(Tr(title_key), nav_list_);
    item->setSizeHint(QSize(175, 42));
    item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    nav_title_keys_.push_back(title_key);
    tabs_.push_back(tab);
    page_stack_->addWidget(tab);
  };

  // Create and register modular tab pages (UserDictTab is kept in codebase for reuse; omitted from UI
  // because FTS5 native phrase matching covers compound words).
  auto *general_tab = new GeneralTab(page_stack_);
  connect(general_tab, &GeneralTab::LanguageChanged, this, &SettingsDialog::OnLanguageChanged);
  register_tab("tab_general", general_tab);
  register_tab("tab_dirs", new DirectoriesTab(page_stack_));
  register_tab("tab_patterns", new PatternsTab(page_stack_));
  register_tab("tab_exts", new ExtensionsTab(page_stack_));
  register_tab("tab_perf", new PerformanceTab(page_stack_));

  body_layout->addWidget(nav_list_);
  body_layout->addWidget(page_stack_, 1);
  main_layout->addWidget(body_widget, 1);

  auto *bottom_container = new QWidget(this);
  bottom_container->setObjectName("settings_bottom_container");
  auto *bottom_bar = CreateBottomBar();
  bottom_container->setLayout(bottom_bar);
  main_layout->addWidget(bottom_container);

  connect(nav_list_, &QListWidget::currentRowChanged, page_stack_, &QStackedWidget::setCurrentIndex);
  nav_list_->setCurrentRow(0);

  RetranslateUi();
}

void SettingsDialog::SetCurrentTab(int index) {
  if (nav_list_ && index >= 0 && index < nav_list_->count()) {
    nav_list_->setCurrentRow(index);
  }
}

QHBoxLayout *SettingsDialog::CreateBottomBar() {
  auto *bottom_layout = new QHBoxLayout();
  btn_rebuild_ = new QPushButton(this);
  btn_rebuild_->setStyleSheet("QPushButton { color: #D32F2F; }");
  bottom_layout->addWidget(btn_rebuild_);
  bottom_layout->addStretch();
  btn_save_ = new QPushButton(this);
  btn_save_->setDefault(true);
  btn_cancel_ = new QPushButton(this);
  bottom_layout->addWidget(btn_save_);
  bottom_layout->addWidget(btn_cancel_);

  connect(btn_rebuild_, &QPushButton::clicked, this, &SettingsDialog::OnRebuildIndex);
  connect(btn_save_, &QPushButton::clicked, this, &SettingsDialog::OnSaveAndApply);
  connect(btn_cancel_, &QPushButton::clicked, this, [this]() {
    if (config_) {
      I18n::Instance().SetLanguage(config_->ui_language());
    }
    reject();
  });

  return bottom_layout;
}

void SettingsDialog::PopulateFromConfig() {
  if (!config_) return;
  for (auto *tab : tabs_) {
    tab->PopulateFromConfig(*config_);
  }
}

void SettingsDialog::RetranslateUi() {
  setWindowTitle(Tr("settings_title"));

  if (nav_list_) {
    const int count = std::min<int>(nav_list_->count(), static_cast<int>(nav_title_keys_.size()));
    for (int i = 0; i < count; ++i) {
      nav_list_->item(i)->setText(Tr(nav_title_keys_[i]));
    }
  }

  if (btn_rebuild_) btn_rebuild_->setText(Tr("btn_rebuild"));
  if (btn_save_) btn_save_->setText(Tr("btn_save"));
  if (btn_cancel_) btn_cancel_->setText(Tr("btn_cancel"));

  for (auto *tab : tabs_) {
    tab->RetranslateUi();
  }
}

void SettingsDialog::OnLanguageChanged(const std::string &) {
  RetranslateUi();
}

void SettingsDialog::OnSaveAndApply() {
  if (!config_) {
    accept();
    return;
  }

  for (auto *tab : tabs_) {
    tab->ApplyToConfig(config_.get());
  }

  QString config_path;
  if (!config_->config_path().empty()) {
    config_path = QString::fromStdString(config_->config_path());
  } else {
    config_path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/config.json";
  }

  if (!config_->SaveToFile(config_path.toStdString())) {
    QMessageBox::warning(this, Tr("dlg_error"), Tr("save_cfg_failed"));
    return;
  }

  emit SettingsApplied();
  accept();
}

void SettingsDialog::OnRebuildIndex() {
  auto reply = QMessageBox::question(this, Tr("rebuild_confirm_title"), Tr("rebuild_confirm_msg"),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (reply == QMessageBox::Yes) {
    emit RebuildIndexRequested();
    accept();
  }
}

}  // namespace gui
}  // namespace soeasy
