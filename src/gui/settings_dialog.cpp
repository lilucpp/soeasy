#include "gui/settings_dialog.h"
#include "gui/i18n.h"

#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTabBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

#include "common/format_categories.h"
#include "common/logger.h"

namespace soeasy {
namespace gui {

SettingsDialog::SettingsDialog(std::shared_ptr<service::IndexConfig> config,
                               QWidget* parent)
    : QDialog(parent), config_(std::move(config)) {
  setWindowTitle("SoEasy - 索引配置与设置中心");
  resize(800, 620);
  SetupUi();
  PopulateFromConfig();
}

void SettingsDialog::SetupUi() {
  setStyleSheet(
      "QDialog {"
      "  background-color: #FFFAFA;"
      "  color: #1E293B;"
      "  font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI', 'PingFang SC', 'Noto Sans CJK SC', 'WenQuanYi Micro Hei', sans-serif;"
      "}"
      "QTabWidget::pane {"
      "  border: 1px solid #E2E8F0;"
      "  background-color: #FFFFFF;"
      "  border-radius: 6px;"
      "  top: -1px;"
      "}"
      "QTabBar::tab {"
      "  background-color: #F1F5F9;"
      "  color: #475569;"
      "  border: 1px solid #CBD5E1;"
      "  border-bottom: none;"
      "  padding: 6px 12px;"
      "  min-width: 125px;"
      "  border-top-left-radius: 6px;"
      "  border-top-right-radius: 6px;"
      "  margin-right: 3px;"
      "  font-size: 12px;"
      "  font-weight: 500;"
      "}"
      "QTabBar::tab:selected {"
      "  background-color: #FFFFFF;"
      "  color: #0284C7;"
      "  border-color: #E2E8F0;"
      "  border-bottom: 1px solid #FFFFFF;"
      "  font-weight: bold;"
      "}"
      "QTabBar::tab:hover:!selected {"
      "  background-color: #E2E8F0;"
      "  color: #0F172A;"
      "}"
      "QTreeWidget, QListWidget, QLineEdit, QSpinBox, QComboBox {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  padding: 4px 6px;"
      "  color: #1E293B;"
      "}"
      "QTreeWidget::item:selected, QListWidget::item:selected {"
      "  background-color: #E0F2FE;"
      "  color: #0369A1;"
      "}"
      "QTreeWidget::item:hover:!selected, QListWidget::item:hover:!selected {"
      "  background-color: #F8FAFC;"
      "}"
      "QHeaderView::section {"
      "  background-color: #F8FAFC;"
      "  color: #475569;"
      "  border: none;"
      "  border-right: 1px solid #E2E8F0;"
      "  border-bottom: 1px solid #E2E8F0;"
      "  padding: 5px 8px;"
      "  font-weight: 600;"
      "}"
      "QPushButton {"
      "  background-color: #F8FAFC;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  padding: 6px 14px;"
      "  color: #334155;"
      "  font-weight: 500;"
      "}"
      "QPushButton:hover {"
      "  background-color: #F1F5F9;"
      "  border-color: #94A3B8;"
      "  color: #0F172A;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #E2E8F0;"
      "}"
      "QPushButton:default {"
      "  background-color: #0284C7;"
      "  border-color: #0369A1;"
      "  color: #FFFFFF;"
      "}"
      "QPushButton:default:hover {"
      "  background-color: #0369A1;"
      "}");

  auto* main_layout = new QVBoxLayout(this);
  tab_widget_ = new QTabWidget(this);
  tab_widget_->setElideMode(Qt::ElideNone);
  if (tab_widget_->tabBar()) {
    tab_widget_->tabBar()->setElideMode(Qt::ElideNone);
    tab_widget_->tabBar()->setExpanding(false);
  }

  // -------------------------------------------------------------
  // Tab 0: 常规与语言 (General & Language)
  // -------------------------------------------------------------
  auto* tab_gen = new QWidget();
  auto* form_gen = new QFormLayout(tab_gen);
  form_gen->setContentsMargins(16, 16, 16, 16);
  form_gen->setSpacing(12);

  label_language_ = new QLabel("界面显示语言 (UI Language):", tab_gen);
  language_combo_ = new QComboBox(tab_gen);
  language_combo_->addItem("简体中文 (Simplified Chinese)", "zh");
  language_combo_->addItem("English", "en");
  form_gen->addRow(label_language_, language_combo_);

  label_lang_hint_ = new QLabel(tab_gen);
  label_lang_hint_->setStyleSheet("color: #64748B; font-size: 12px; margin-top: 4px;");
  form_gen->addRow("", label_lang_hint_);

  tab_widget_->addTab(tab_gen, "常规与语言");

  connect(language_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    std::string chosen_lang = language_combo_->currentData().toString().toStdString();
    I18n::Instance().SetLanguage(chosen_lang);
    RetranslateUi();
  });

  // -------------------------------------------------------------
  // Tab 1: 索引监控目录 (Indexed Directories)
  // -------------------------------------------------------------
  auto* tab_dirs = new QWidget();
  auto* layout_dirs = new QVBoxLayout(tab_dirs);

  label_dirs_ = new QLabel("系统将实时监控并自动为以下目录及其子文件夹建立索引：", tab_dirs);
  dirs_list_ = new QListWidget(tab_dirs);

  auto* btn_layout_dirs = new QHBoxLayout();
  btn_add_dir_ = new QPushButton("添加目录...", tab_dirs);
  btn_rm_dir_ = new QPushButton("移除所选", tab_dirs);
  btn_layout_dirs->addWidget(btn_add_dir_);
  btn_layout_dirs->addWidget(btn_rm_dir_);
  btn_layout_dirs->addStretch();

  layout_dirs->addWidget(label_dirs_);
  layout_dirs->addWidget(dirs_list_);
  layout_dirs->addLayout(btn_layout_dirs);
  tab_widget_->addTab(tab_dirs, "索引目录");

  connect(btn_add_dir_, &QPushButton::clicked, this, &SettingsDialog::OnAddDirectory);
  connect(btn_rm_dir_, &QPushButton::clicked, this, &SettingsDialog::OnRemoveDirectory);

  // -------------------------------------------------------------
  // Tab 2: 排除目录与规则 (Excluded Patterns)
  // -------------------------------------------------------------
  auto* tab_patterns = new QWidget();
  auto* layout_patterns = new QVBoxLayout(tab_patterns);

  label_patterns_ = new QLabel("匹配以下路径规则的目录将被自动忽略（不建立倒排索引）：", tab_patterns);
  patterns_list_ = new QListWidget(tab_patterns);

  auto* add_pattern_layout = new QHBoxLayout();
  new_pattern_edit_ = new QLineEdit(tab_patterns);
  new_pattern_edit_->setPlaceholderText("例如: /.git/ 或 /node_modules/");
  btn_add_pattern_ = new QPushButton("添加规则", tab_patterns);
  btn_rm_pattern_ = new QPushButton("移除所选", tab_patterns);
  add_pattern_layout->addWidget(new_pattern_edit_);
  add_pattern_layout->addWidget(btn_add_pattern_);
  add_pattern_layout->addWidget(btn_rm_pattern_);

  layout_patterns->addWidget(label_patterns_);
  layout_patterns->addWidget(patterns_list_);
  layout_patterns->addLayout(add_pattern_layout);
  tab_widget_->addTab(tab_patterns, "排除目录规则");

  connect(btn_add_pattern_, &QPushButton::clicked, this, &SettingsDialog::OnAddPattern);
  connect(btn_rm_pattern_, &QPushButton::clicked, this, &SettingsDialog::OnRemovePattern);

  // -------------------------------------------------------------
  // Tab 3: 支持与自定义文件格式 (File Extensions & Categories)
  // -------------------------------------------------------------
  auto* tab_exts = new QWidget();
  auto* layout_exts = new QVBoxLayout(tab_exts);

  // Group 1: 嵌入图片解析选项
  group_ocr_ = new QGroupBox("高级解析能力", tab_exts);
  auto* layout_ocr = new QVBoxLayout(group_ocr_);
  embedded_image_ocr_check_ = new QCheckBox(
      "提取并 OCR 解析文档内嵌图片 (Office 系列 / PDF / ODF 等)", group_ocr_);
  QFont ocr_font = embedded_image_ocr_check_->font();
  ocr_font.setBold(true);
  embedded_image_ocr_check_->setFont(ocr_font);
  label_ocr_tip_ = new QLabel(
      "💡 默认不勾选。勾选后将提取 Office/PDF/ODF 等文档中的嵌入插图并调用 PP-OCRv6 引擎进行文本识别融合；\n"
      "    不勾选可获得极致解析性能（纯文本毫秒级完成索引）。", group_ocr_);
  label_ocr_tip_->setStyleSheet("color: #666; font-size: 12px; margin-left: 20px;");
  layout_ocr->addWidget(embedded_image_ocr_check_);
  layout_ocr->addWidget(label_ocr_tip_);
  layout_exts->addWidget(group_ocr_);

  // Group 2: 文件分类树
  label_tree_title_ = new QLabel("支持与索引的文件格式分类（勾选以启用全文索引）：", tab_exts);
  layout_exts->addWidget(label_tree_title_);

  exts_tree_ = new QTreeWidget(tab_exts);
  exts_tree_->setColumnCount(2);
  exts_tree_->setHeaderLabels({"分类 / 格式扩展名", "说明与处理引擎"});
  exts_tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  exts_tree_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  exts_tree_->setAlternatingRowColors(true);
  layout_exts->addWidget(exts_tree_);

  // Populate preset categories into tree
  size_t cat_idx = 0;
  for (const auto& cat : common::GetPresetCategories()) {
    auto* cat_item = new QTreeWidgetItem(exts_tree_);
    cat_item->setData(0, Qt::UserRole + 1, static_cast<qulonglong>(cat_idx));
    cat_item->setText(0, I18n::Instance().GetCategoryName(cat_idx, QString::fromUtf8(cat.name)));
    cat_item->setText(1, I18n::Instance().GetCategoryDesc(cat_idx, QString::fromUtf8(cat.desc)));
    cat_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    QFont cat_font = cat_item->font(0);
    cat_font.setBold(true);
    cat_item->setFont(0, cat_font);

    for (const auto& fmt : cat.formats) {
      auto* fmt_item = new QTreeWidgetItem(cat_item);
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
  auto* add_custom_layout = new QHBoxLayout();
  label_custom_ = new QLabel("添加自定义扩展名:", tab_exts);
  new_custom_ext_edit_ = new QLineEdit(tab_exts);
  new_custom_ext_edit_->setPlaceholderText("例如: vue 或 proto (无需加点)");
  btn_add_custom_ext_ = new QPushButton("添加格式", tab_exts);
  btn_rm_custom_ext_ = new QPushButton("移除所选自定义", tab_exts);

  add_custom_layout->addWidget(label_custom_);
  add_custom_layout->addWidget(new_custom_ext_edit_);
  add_custom_layout->addWidget(btn_add_custom_ext_);
  add_custom_layout->addWidget(btn_rm_custom_ext_);
  layout_exts->addLayout(add_custom_layout);

  // Bottom action bar
  auto* bottom_exts_layout = new QHBoxLayout();
  btn_select_all_ = new QPushButton("全选", tab_exts);
  btn_deselect_all_ = new QPushButton("全不选", tab_exts);
  btn_reset_exts_ = new QPushButton("恢复推荐默认", tab_exts);
  exts_count_label_ = new QLabel("已启用: 0 种文件格式", tab_exts);
  exts_count_label_->setStyleSheet("font-weight: bold; color: #1976D2;");

  bottom_exts_layout->addWidget(btn_select_all_);
  bottom_exts_layout->addWidget(btn_deselect_all_);
  bottom_exts_layout->addWidget(btn_reset_exts_);
  bottom_exts_layout->addStretch();
  bottom_exts_layout->addWidget(exts_count_label_);
  layout_exts->addLayout(bottom_exts_layout);

  tab_widget_->addTab(tab_exts, "文件格式与解析");

  connect(btn_add_custom_ext_, &QPushButton::clicked, this, &SettingsDialog::OnAddCustomExtension);
  connect(btn_rm_custom_ext_, &QPushButton::clicked, this, &SettingsDialog::OnRemoveCustomExtension);
  connect(btn_select_all_, &QPushButton::clicked, this, &SettingsDialog::OnSelectAllExtensions);
  connect(btn_deselect_all_, &QPushButton::clicked, this, &SettingsDialog::OnDeselectAllExtensions);
  connect(btn_reset_exts_, &QPushButton::clicked, this, &SettingsDialog::OnResetExtensionsToDefault);
  connect(exts_tree_, &QTreeWidget::itemChanged, this, &SettingsDialog::OnTreeItemChanged);

  // -------------------------------------------------------------
  // Tab 4: 性能与调度参数 (Performance)
  // -------------------------------------------------------------
  auto* tab_perf = new QWidget();
  auto* form_perf = new QFormLayout(tab_perf);

  debounce_spin_ = new QSpinBox(tab_perf);
  debounce_spin_->setRange(50, 5000);
  debounce_spin_->setSingleStep(50);
  debounce_spin_->setSuffix(" 毫秒");
  label_debounce_ = new QLabel("文件变动去抖动等待延迟:", tab_perf);
  form_perf->addRow(label_debounce_, debounce_spin_);

  max_size_spin_ = new QSpinBox(tab_perf);
  max_size_spin_->setRange(1, 1024);
  max_size_spin_->setSingleStep(10);
  max_size_spin_->setSuffix(" MB");
  label_max_size_ = new QLabel("单文件最大全文索引上限:", tab_perf);
  form_perf->addRow(label_max_size_, max_size_spin_);

  worker_count_spin_ = new QSpinBox(tab_perf);
  worker_count_spin_->setRange(1, 8);
  worker_count_spin_->setSingleStep(1);
  worker_count_spin_->setSuffix(" 个进程");
  label_worker_count_ = new QLabel("并发解析 Worker 进程数:", tab_perf);
  form_perf->addRow(label_worker_count_, worker_count_spin_);

  min_img_chars_spin_ = new QSpinBox(tab_perf);
  min_img_chars_spin_->setRange(0, 50);
  min_img_chars_spin_->setSingleStep(1);
  min_img_chars_spin_->setSuffix(" 个字 (0为不限制)");
  label_min_img_chars_ = new QLabel("图片最少有效文字数（过滤表情/图标）:", tab_perf);
  form_perf->addRow(label_min_img_chars_, min_img_chars_spin_);

  filter_thumbs_check_ = new QCheckBox("自动忽略应用缓存缩略图与图标 (如 WeAppIcon, Thumb)", tab_perf);
  form_perf->addRow("", filter_thumbs_check_);

  log_level_combo_ = new QComboBox(tab_perf);
  log_level_combo_->addItem("DEBUG (详细调试与耗时跟踪)", "debug");
  log_level_combo_->addItem("INFO  (标准运行信息，默认)", "info");
  log_level_combo_->addItem("WARN  (生产精简模式，仅警告/错误)", "warn");
  log_level_combo_->addItem("ERROR (仅严重故障记录)", "error");
  label_log_level_ = new QLabel("运行日志详细级别:", tab_perf);
  form_perf->addRow(label_log_level_, log_level_combo_);

  log_console_check_ = new QCheckBox("输出控制台实时镜像 (Console stdout，用于终端调试)", tab_perf);
  form_perf->addRow("终端输出调试:", log_console_check_);

  log_sync_check_ = new QCheckBox("同步即时落盘模式 (Sync Flush，每条日志即刻刷盘，排查异常卡死)", tab_perf);
  form_perf->addRow("落盘同步模式:", log_sync_check_);

  doctotext_log_check_ = new QCheckBox("启用 DocToText 详细文档解析日志 (默认关闭，用于排查 Office/PDF 解析问题)", tab_perf);
  form_perf->addRow("DocToText 日志:", doctotext_log_check_);

  auto* dt_path_layout = new QHBoxLayout();
  doctotext_log_path_edit_ = new QLineEdit(tab_perf);
  doctotext_log_path_edit_->setPlaceholderText("留空则默认输出至 logs/doctotext.log");
  dt_browse_btn_ = new QPushButton("浏览...", tab_perf);
  dt_browse_btn_->setFixedWidth(70);
  dt_path_layout->addWidget(doctotext_log_path_edit_);
  dt_path_layout->addWidget(dt_browse_btn_);
  label_dt_path_ = new QLabel("日志输出文件路径:", tab_perf);
  form_perf->addRow(label_dt_path_, dt_path_layout);

  connect(dt_browse_btn_, &QPushButton::clicked, this, &SettingsDialog::OnBrowseDocToTextLogPath);

  tab_widget_->addTab(tab_perf, "性能与日志");

  main_layout->addWidget(tab_widget_);

  // -------------------------------------------------------------
  // Bottom Buttons: 重建索引 / 保存 / 取消
  // -------------------------------------------------------------
  auto* bottom_layout = new QHBoxLayout();
  btn_rebuild_ = new QPushButton("清空并重建索引...", this);
  btn_rebuild_->setStyleSheet("QPushButton { color: #D32F2F; font-weight: bold; }");
  bottom_layout->addWidget(btn_rebuild_);
  bottom_layout->addStretch();
  btn_save_ = new QPushButton("保存并应用", this);
  btn_save_->setDefault(true);
  btn_cancel_ = new QPushButton("取消", this);
  bottom_layout->addWidget(btn_save_);
  bottom_layout->addWidget(btn_cancel_);
  main_layout->addLayout(bottom_layout);

  connect(btn_rebuild_, &QPushButton::clicked, this, &SettingsDialog::OnRebuildIndex);
  connect(btn_save_, &QPushButton::clicked, this, &SettingsDialog::OnSaveAndApply);
  connect(btn_cancel_, &QPushButton::clicked, this, [this]() {
    if (config_) {
      I18n::Instance().SetLanguage(config_->ui_language());
    }
    reject();
  });

  RetranslateUi();
}

void SettingsDialog::RetranslateUi() {
  setWindowTitle(Tr("settings_title"));
  if (tab_widget_) {
    tab_widget_->setTabText(0, Tr("tab_general"));
    tab_widget_->setTabText(1, Tr("tab_dirs"));
    tab_widget_->setTabText(2, Tr("tab_patterns"));
    tab_widget_->setTabText(3, Tr("tab_exts"));
    tab_widget_->setTabText(4, Tr("tab_perf"));
  }

  // Tab 0
  if (label_language_) label_language_->setText(Tr("label_language"));
  if (language_combo_) {
    int cur_idx = language_combo_->currentIndex();
    language_combo_->blockSignals(true);
    language_combo_->setItemText(0, Tr("lang_zh"));
    language_combo_->setItemText(1, Tr("lang_en"));
    if (cur_idx >= 0) language_combo_->setCurrentIndex(cur_idx);
    language_combo_->blockSignals(false);
  }
  if (label_lang_hint_) label_lang_hint_->setText(Tr("lang_hint"));

  // Tab 1
  if (label_dirs_) label_dirs_->setText(Tr("label_dirs"));
  if (btn_add_dir_) btn_add_dir_->setText(Tr("btn_add_dir"));
  if (btn_rm_dir_) btn_rm_dir_->setText(Tr("btn_rm_dir"));

  // Tab 2
  if (label_patterns_) label_patterns_->setText(Tr("label_patterns"));
  if (new_pattern_edit_) new_pattern_edit_->setPlaceholderText(Tr("pattern_ph"));
  if (btn_add_pattern_) btn_add_pattern_->setText(Tr("btn_add_pattern"));
  if (btn_rm_pattern_) btn_rm_pattern_->setText(Tr("btn_rm_dir"));

  // Tab 3
  if (group_ocr_) group_ocr_->setTitle(Tr("group_ocr"));
  if (embedded_image_ocr_check_) embedded_image_ocr_check_->setText(Tr("check_embedded_ocr"));
  if (label_ocr_tip_) label_ocr_tip_->setText(Tr("tip_embedded_ocr"));
  if (label_tree_title_) label_tree_title_->setText(Tr("label_tree_title"));
  if (exts_tree_) {
    exts_tree_->setHeaderLabels({Tr("tree_header_col0"), Tr("tree_header_col1")});
    for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
      auto* cat_item = exts_tree_->topLevelItem(i);
      if (cat_item == custom_category_item_) {
        cat_item->setText(0, Tr("custom_category_name"));
        cat_item->setText(1, Tr("custom_category_desc"));
        for (int c = 0; c < cat_item->childCount(); ++c) {
          auto* child = cat_item->child(c);
          child->setText(1, Tr("custom_ext_desc"));
        }
      } else {
        QVariant v_idx = cat_item->data(0, Qt::UserRole + 1);
        if (v_idx.isValid()) {
          size_t c_idx = v_idx.toULongLong();
          cat_item->setText(0, I18n::Instance().GetCategoryName(c_idx, cat_item->text(0)));
          cat_item->setText(1, I18n::Instance().GetCategoryDesc(c_idx, cat_item->text(1)));
        }
        for (int c = 0; c < cat_item->childCount(); ++c) {
          auto* fmt_item = cat_item->child(c);
          QString ext = fmt_item->data(0, Qt::UserRole).toString();
          QString default_desc = fmt_item->data(1, Qt::UserRole).toString();
          bool is_planned = fmt_item->data(1, Qt::UserRole + 1).toBool();
          QString desc = I18n::Instance().GetFormatDesc(ext, default_desc);
          if (is_planned) {
            desc += Tr("format_planned");
            fmt_item->setToolTip(0, Tr("cat_disabled_tip"));
            fmt_item->setToolTip(1, Tr("cat_disabled_tip"));
          }
          fmt_item->setText(1, desc);
        }
      }
    }
  }
  if (label_custom_) label_custom_->setText(Tr("label_add_custom"));
  if (new_custom_ext_edit_) new_custom_ext_edit_->setPlaceholderText(Tr("custom_ext_ph"));
  if (btn_add_custom_ext_) btn_add_custom_ext_->setText(Tr("btn_add_custom"));
  if (btn_rm_custom_ext_) btn_rm_custom_ext_->setText(Tr("btn_rm_custom"));
  if (btn_select_all_) btn_select_all_->setText(Tr("btn_select_all"));
  if (btn_deselect_all_) btn_deselect_all_->setText(Tr("btn_deselect_all"));
  if (btn_reset_exts_) btn_reset_exts_->setText(Tr("btn_reset_exts"));
  UpdateExtensionsCount();

  // Tab 4
  if (label_debounce_) label_debounce_->setText(Tr("label_debounce"));
  if (debounce_spin_) debounce_spin_->setSuffix(Tr("suffix_ms"));
  if (label_max_size_) label_max_size_->setText(Tr("label_max_file_size"));
  if (max_size_spin_) max_size_spin_->setSuffix(Tr("suffix_mb"));
  if (label_worker_count_) label_worker_count_->setText(Tr("label_workers"));
  if (worker_count_spin_) worker_count_spin_->setSuffix(Tr("suffix_workers"));
  if (label_min_img_chars_) label_min_img_chars_->setText(Tr("label_min_ocr_chars"));
  if (min_img_chars_spin_) min_img_chars_spin_->setSuffix(Tr("suffix_min_chars"));
  if (filter_thumbs_check_) filter_thumbs_check_->setText(Tr("check_filter_thumbs"));

  if (label_log_level_) label_log_level_->setText(Tr("label_log_level"));
  if (log_level_combo_) {
    int cur_idx = log_level_combo_->currentIndex();
    log_level_combo_->setItemText(0, Tr("log_debug"));
    log_level_combo_->setItemText(1, Tr("log_info"));
    log_level_combo_->setItemText(2, Tr("log_warn"));
    log_level_combo_->setItemText(3, Tr("log_error"));
    if (cur_idx >= 0) log_level_combo_->setCurrentIndex(cur_idx);
  }
  if (log_console_check_) log_console_check_->setText(Tr("check_log_console"));
  if (log_sync_check_) log_sync_check_->setText(Tr("check_log_sync"));
  if (doctotext_log_check_) doctotext_log_check_->setText(Tr("check_dt_log"));
  if (label_dt_path_) label_dt_path_->setText(Tr("label_dt_path"));
  if (doctotext_log_path_edit_) doctotext_log_path_edit_->setPlaceholderText(Tr("dt_path_ph"));
  if (dt_browse_btn_) dt_browse_btn_->setText(Tr("browse"));

  // Bottom buttons
  if (btn_rebuild_) btn_rebuild_->setText(Tr("btn_rebuild"));
  if (btn_save_) btn_save_->setText(Tr("save_apply"));
  if (btn_cancel_) btn_cancel_->setText(Tr("cancel"));
}

void SettingsDialog::OnRebuildIndex() {
  auto reply = QMessageBox::warning(
      this, Tr("rebuild_confirm_title"),
      Tr("rebuild_confirm_text"),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (reply == QMessageBox::Yes) {
    emit RebuildIndexRequested();
    accept();
  }
}

void SettingsDialog::PopulateFromConfig() {
  if (!config_) {
    return;
  }

  if (language_combo_) {
    int lang_idx = language_combo_->findData(QString::fromStdString(config_->ui_language()));
    if (lang_idx >= 0) {
      language_combo_->setCurrentIndex(lang_idx);
    }
  }

  dirs_list_->clear();
  for (const auto& dir : config_->GetIndexedDirectories()) {
    dirs_list_->addItem(QString::fromStdString(dir));
  }

  patterns_list_->clear();
  for (const auto& pat : config_->GetExcludedPatterns()) {
    patterns_list_->addItem(QString::fromStdString(pat));
  }

  // Populate Tab 3: Embedded image OCR check and extensions tree
  embedded_image_ocr_check_->setChecked(config_->enable_embedded_image_ocr());

  is_updating_tree_ = true;
  auto supported_exts = config_->GetSupportedExtensions();
  auto custom_exts = config_->GetCustomExtensions();

  // Clear existing custom items from custom_category_item_
  while (custom_category_item_->childCount() > 0) {
    delete custom_category_item_->takeChild(0);
  }

  // Populate custom extensions
  for (const auto& c_ext : custom_exts) {
    auto* item = new QTreeWidgetItem(custom_category_item_);
    QString q_ext = QString::fromStdString(c_ext);
    item->setText(0, q_ext);
    item->setText(1, Tr("custom_ext_desc"));
    item->setData(0, Qt::UserRole, q_ext);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
    bool is_checked = (supported_exts.find(c_ext) != supported_exts.end());
    item->setCheckState(0, is_checked ? Qt::Checked : Qt::Unchecked);
  }

  // Match check states across all categories in tree
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto* cat_item = exts_tree_->topLevelItem(i);
    if (cat_item == custom_category_item_) continue;
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto* fmt_item = cat_item->child(j);
      if (fmt_item->isDisabled()) {
        fmt_item->setCheckState(0, Qt::Unchecked);
        continue;
      }
      std::string ext = fmt_item->data(0, Qt::UserRole).toString().toStdString();
      bool is_checked = (supported_exts.find(ext) != supported_exts.end());
      fmt_item->setCheckState(0, is_checked ? Qt::Checked : Qt::Unchecked);
    }
  }

  is_updating_tree_ = false;
  UpdateExtensionsCount();

  debounce_spin_->setValue(static_cast<int>(config_->debounce_ms()));
  max_size_spin_->setValue(static_cast<int>(config_->max_file_size() / (1024 * 1024)));
  worker_count_spin_->setValue(static_cast<int>(config_->worker_count()));
  min_img_chars_spin_->setValue(static_cast<int>(config_->min_image_text_length()));
  filter_thumbs_check_->setChecked(config_->filter_cache_thumbnails());

  int log_idx = log_level_combo_->findData(QString::fromStdString(config_->log_level()));
  if (log_idx >= 0) {
    log_level_combo_->setCurrentIndex(log_idx);
  } else {
    log_level_combo_->setCurrentIndex(1);  // default info
  }
  log_console_check_->setChecked(config_->log_console());
  log_sync_check_->setChecked(config_->log_sync());
  doctotext_log_check_->setChecked(config_->doctotext_logging());
  doctotext_log_path_edit_->setText(QString::fromStdString(config_->doctotext_log_path()));
}

void SettingsDialog::OnAddDirectory() {
  QString dir = QFileDialog::getExistingDirectory(
      this, Tr("dlg_select_dir"), QDir::homePath(),
      QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
  if (!dir.isEmpty()) {
    // Check duplicate
    for (int i = 0; i < dirs_list_->count(); ++i) {
      if (dirs_list_->item(i)->text() == dir) {
        return;
      }
    }
    dirs_list_->addItem(dir);
  }
}

void SettingsDialog::OnRemoveDirectory() {
  int row = dirs_list_->currentRow();
  if (row >= 0) {
    delete dirs_list_->takeItem(row);
  }
}

void SettingsDialog::OnAddPattern() {
  QString pattern = new_pattern_edit_->text().trimmed();
  if (!pattern.isEmpty()) {
    patterns_list_->addItem(pattern);
    new_pattern_edit_->clear();
  }
}

void SettingsDialog::OnRemovePattern() {
  int row = patterns_list_->currentRow();
  if (row >= 0) {
    delete patterns_list_->takeItem(row);
  }
}

void SettingsDialog::UpdateExtensionsCount() {
  int total_formats = 0;
  int enabled_formats = 0;

  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto* cat_item = exts_tree_->topLevelItem(i);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      total_formats++;
      if (cat_item->child(j)->checkState(0) == Qt::Checked) {
        enabled_formats++;
      }
    }
  }

  if (exts_count_label_) {
    exts_count_label_->setText(Tr("exts_count_status")
                                   .arg(enabled_formats)
                                   .arg(total_formats));
  }
}

void SettingsDialog::OnTreeItemChanged(QTreeWidgetItem* item, int column) {
  if (column != 0 || is_updating_tree_) return;
  is_updating_tree_ = true;

  if (item->childCount() > 0) {
    // If parent category item was clicked/toggled, sync state to all its children
    Qt::CheckState state = item->checkState(0);
    if (state == Qt::Checked || state == Qt::Unchecked) {
      for (int i = 0; i < item->childCount(); ++i) {
        if (!item->child(i)->isDisabled()) {
          item->child(i)->setCheckState(0, state);
        }
      }
    }
  }

  is_updating_tree_ = false;
  UpdateExtensionsCount();
}

void SettingsDialog::OnSelectAllExtensions() {
  is_updating_tree_ = true;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto* cat_item = exts_tree_->topLevelItem(i);
    if (!cat_item->isDisabled()) {
      cat_item->setCheckState(0, Qt::Checked);
    }
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto* child = cat_item->child(j);
      if (!child->isDisabled()) {
        child->setCheckState(0, Qt::Checked);
      }
    }
  }
  is_updating_tree_ = false;
  UpdateExtensionsCount();
}

void SettingsDialog::OnDeselectAllExtensions() {
  is_updating_tree_ = true;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto* cat_item = exts_tree_->topLevelItem(i);
    cat_item->setCheckState(0, Qt::Unchecked);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      cat_item->child(j)->setCheckState(0, Qt::Unchecked);
    }
  }
  is_updating_tree_ = false;
  UpdateExtensionsCount();
}

void SettingsDialog::OnResetExtensionsToDefault() {
  is_updating_tree_ = true;
  embedded_image_ocr_check_->setChecked(false);
  auto default_exts = service::IndexConfig::GetDefaultSupportedExtensions();

  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto* cat_item = exts_tree_->topLevelItem(i);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto* fmt_item = cat_item->child(j);
      if (fmt_item->isDisabled()) {
        fmt_item->setCheckState(0, Qt::Unchecked);
        continue;
      }
      std::string ext = fmt_item->data(0, Qt::UserRole).toString().toStdString();
      bool is_def = (default_exts.find(ext) != default_exts.end());
      fmt_item->setCheckState(0, is_def ? Qt::Checked : Qt::Unchecked);
    }
  }
  is_updating_tree_ = false;
  UpdateExtensionsCount();
}

void SettingsDialog::OnAddCustomExtension() {
  QString ext = new_custom_ext_edit_->text().trimmed().toLower();
  if (ext.startsWith('.')) {
    ext = ext.mid(1);
  }
  if (ext.isEmpty()) return;

  // Check if extension already exists in tree
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto* cat_item = exts_tree_->topLevelItem(i);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      if (cat_item->child(j)->data(0, Qt::UserRole).toString().toLower() == ext) {
        cat_item->child(j)->setCheckState(0, Qt::Checked);
        exts_tree_->setCurrentItem(cat_item->child(j));
        exts_tree_->scrollToItem(cat_item->child(j));
        new_custom_ext_edit_->clear();
        UpdateExtensionsCount();
        return;
      }
    }
  }

  is_updating_tree_ = true;
  auto* item = new QTreeWidgetItem(custom_category_item_);
  item->setText(0, ext);
  item->setText(1, Tr("custom_ext_desc"));
  item->setData(0, Qt::UserRole, ext);
  item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
  item->setCheckState(0, Qt::Checked);

  custom_category_item_->setExpanded(true);
  is_updating_tree_ = false;
  new_custom_ext_edit_->clear();
  UpdateExtensionsCount();
}

void SettingsDialog::OnRemoveCustomExtension() {
  auto* current = exts_tree_->currentItem();
  if (!current) {
    QMessageBox::information(this, Tr("msg_tip"), Tr("msg_select_custom_first"));
    return;
  }

  if (current->parent() == custom_category_item_) {
    delete current;
    UpdateExtensionsCount();
  } else {
    QMessageBox::information(this, Tr("msg_tip"), Tr("msg_preset_cannot_delete"));
  }
}

void SettingsDialog::OnBrowseDocToTextLogPath() {
  QString cur_path = doctotext_log_path_edit_->text().trimmed();
  if (cur_path.isEmpty()) {
    cur_path = QDir::homePath() + "/doctotext.log";
  }
  QString file = QFileDialog::getSaveFileName(
      this, Tr("dlg_select_log"), cur_path,
      "Log Files (*.log);;Text Files (*.txt);;All Files (*)");
  if (!file.isEmpty()) {
    doctotext_log_path_edit_->setText(file);
  }
}

void SettingsDialog::OnSaveAndApply() {
  if (!config_) {
    accept();
    return;
  }

  // Save UI language
  if (language_combo_) {
    std::string chosen_lang = language_combo_->currentData().toString().toStdString();
    config_->set_ui_language(chosen_lang);
    I18n::Instance().SetLanguage(chosen_lang);
  }

  // Save directories
  std::vector<std::string> dirs;
  for (int i = 0; i < dirs_list_->count(); ++i) {
    dirs.push_back(dirs_list_->item(i)->text().toStdString());
  }
  config_->SetIndexedDirectories(dirs);

  // Save excluded patterns
  std::vector<std::string> patterns;
  for (int i = 0; i < patterns_list_->count(); ++i) {
    patterns.push_back(patterns_list_->item(i)->text().toStdString());
  }
  config_->SetExcludedPatterns(patterns);

  // Save extensions from tree
  std::vector<std::string> exts;
  std::vector<std::string> custom_exts;

  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto* cat_item = exts_tree_->topLevelItem(i);
    bool is_custom = (cat_item == custom_category_item_);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto* fmt_item = cat_item->child(j);
      if (fmt_item->isDisabled()) {
        continue;
      }
      std::string ext = fmt_item->data(0, Qt::UserRole).toString().toStdString();
      if (fmt_item->checkState(0) == Qt::Checked) {
        exts.push_back(ext);
      }
      if (is_custom) {
        custom_exts.push_back(ext);
      }
    }
  }
  config_->SetSupportedExtensions(exts);
  config_->SetCustomExtensions(custom_exts);
  config_->set_enable_embedded_image_ocr(embedded_image_ocr_check_->isChecked());

  // Save performance and filtering
  config_->set_debounce_ms(static_cast<uint32_t>(debounce_spin_->value()));
  config_->set_max_file_size(static_cast<uint64_t>(max_size_spin_->value()) * 1024 * 1024);
  config_->set_worker_count(static_cast<size_t>(worker_count_spin_->value()));
  config_->set_min_image_text_length(static_cast<uint32_t>(min_img_chars_spin_->value()));
  config_->set_filter_cache_thumbnails(filter_thumbs_check_->isChecked());

  // Save logging settings
  std::string chosen_level = log_level_combo_->currentData().toString().toStdString();
  bool chosen_console = log_console_check_->isChecked();
  bool chosen_sync = log_sync_check_->isChecked();
  config_->set_log_level(chosen_level);
  config_->set_log_console(chosen_console);
  config_->set_log_sync(chosen_sync);
  config_->set_doctotext_logging(doctotext_log_check_->isChecked());
  config_->set_doctotext_log_path(doctotext_log_path_edit_->text().trimmed().toStdString());

  // Apply immediately to current process
  common::Logger::SetLevel(chosen_level);
  common::Logger::SetConsoleEnabled(chosen_console);
  common::Logger::SetSyncMode(chosen_sync);

  // Persist to disk
  config_->Save();

  emit SettingsApplied();
  accept();
}

}  // namespace gui
}  // namespace soeasy
