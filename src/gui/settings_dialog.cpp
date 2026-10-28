#include "gui/settings_dialog.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

#include "common/logger.h"

namespace soeasy {
namespace gui {

SettingsDialog::SettingsDialog(std::shared_ptr<service::IndexConfig> config,
                               QWidget* parent)
    : QDialog(parent), config_(std::move(config)) {
  setWindowTitle("SoEasy - 索引配置与设置中心");
  resize(640, 480);
  SetupUi();
  PopulateFromConfig();
}

void SettingsDialog::SetupUi() {
  auto* main_layout = new QVBoxLayout(this);
  tab_widget_ = new QTabWidget(this);

  // -------------------------------------------------------------
  // Tab 1: 索引监控目录 (Indexed Directories)
  // -------------------------------------------------------------
  auto* tab_dirs = new QWidget();
  auto* layout_dirs = new QVBoxLayout(tab_dirs);

  auto* label_dirs = new QLabel("系统将实时监控并自动为以下目录及其子文件夹建立索引：", tab_dirs);
  dirs_list_ = new QListWidget(tab_dirs);

  auto* btn_layout_dirs = new QHBoxLayout();
  auto* btn_add_dir = new QPushButton("添加目录...", tab_dirs);
  auto* btn_rm_dir = new QPushButton("移除所选", tab_dirs);
  btn_layout_dirs->addWidget(btn_add_dir);
  btn_layout_dirs->addWidget(btn_rm_dir);
  btn_layout_dirs->addStretch();

  layout_dirs->addWidget(label_dirs);
  layout_dirs->addWidget(dirs_list_);
  layout_dirs->addLayout(btn_layout_dirs);
  tab_widget_->addTab(tab_dirs, "索引目录");

  connect(btn_add_dir, &QPushButton::clicked, this, &SettingsDialog::OnAddDirectory);
  connect(btn_rm_dir, &QPushButton::clicked, this, &SettingsDialog::OnRemoveDirectory);

  // -------------------------------------------------------------
  // Tab 2: 排除目录与规则 (Excluded Patterns)
  // -------------------------------------------------------------
  auto* tab_patterns = new QWidget();
  auto* layout_patterns = new QVBoxLayout(tab_patterns);

  auto* label_patterns = new QLabel("匹配以下路径规则的目录将被自动忽略（不建立倒排索引）：", tab_patterns);
  patterns_list_ = new QListWidget(tab_patterns);

  auto* add_pattern_layout = new QHBoxLayout();
  new_pattern_edit_ = new QLineEdit(tab_patterns);
  new_pattern_edit_->setPlaceholderText("例如: /.git/ 或 /node_modules/");
  auto* btn_add_pattern = new QPushButton("添加规则", tab_patterns);
  auto* btn_rm_pattern = new QPushButton("移除所选", tab_patterns);
  add_pattern_layout->addWidget(new_pattern_edit_);
  add_pattern_layout->addWidget(btn_add_pattern);
  add_pattern_layout->addWidget(btn_rm_pattern);

  layout_patterns->addWidget(label_patterns);
  layout_patterns->addWidget(patterns_list_);
  layout_patterns->addLayout(add_pattern_layout);
  tab_widget_->addTab(tab_patterns, "排除目录规则");

  connect(btn_add_pattern, &QPushButton::clicked, this, &SettingsDialog::OnAddPattern);
  connect(btn_rm_pattern, &QPushButton::clicked, this, &SettingsDialog::OnRemovePattern);

  // -------------------------------------------------------------
  // Tab 3: 支持与自定义文件格式 (File Extensions)
  // -------------------------------------------------------------
  auto* tab_exts = new QWidget();
  auto* layout_exts = new QVBoxLayout(tab_exts);

  auto* label_exts = new QLabel("允许被解析与索引的文件扩展名格式列表：", tab_exts);
  exts_list_ = new QListWidget(tab_exts);

  auto* add_ext_layout = new QHBoxLayout();
  new_ext_edit_ = new QLineEdit(tab_exts);
  new_ext_edit_->setPlaceholderText("例如: log 或 v");
  auto* btn_add_ext = new QPushButton("添加格式", tab_exts);
  auto* btn_rm_ext = new QPushButton("移除所选", tab_exts);
  auto* btn_reset_exts = new QPushButton("恢复默认推荐格式", tab_exts);
  add_ext_layout->addWidget(new_ext_edit_);
  add_ext_layout->addWidget(btn_add_ext);
  add_ext_layout->addWidget(btn_rm_ext);
  add_ext_layout->addWidget(btn_reset_exts);

  layout_exts->addWidget(label_exts);
  layout_exts->addWidget(exts_list_);
  layout_exts->addLayout(add_ext_layout);
  tab_widget_->addTab(tab_exts, "文件格式");

  connect(btn_add_ext, &QPushButton::clicked, this, &SettingsDialog::OnAddExtension);
  connect(btn_rm_ext, &QPushButton::clicked, this, &SettingsDialog::OnRemoveExtension);
  connect(btn_reset_exts, &QPushButton::clicked, this, &SettingsDialog::OnResetExtensionsToDefault);

  // -------------------------------------------------------------
  // Tab 4: 性能与调度参数 (Performance)
  // -------------------------------------------------------------
  auto* tab_perf = new QWidget();
  auto* form_perf = new QFormLayout(tab_perf);

  debounce_spin_ = new QSpinBox(tab_perf);
  debounce_spin_->setRange(50, 5000);
  debounce_spin_->setSingleStep(50);
  debounce_spin_->setSuffix(" 毫秒");
  form_perf->addRow("文件变动去抖动等待延迟:", debounce_spin_);

  max_size_spin_ = new QSpinBox(tab_perf);
  max_size_spin_->setRange(1, 1024);
  max_size_spin_->setSingleStep(10);
  max_size_spin_->setSuffix(" MB");
  form_perf->addRow("单文件最大全文索引上限:", max_size_spin_);

  worker_count_spin_ = new QSpinBox(tab_perf);
  worker_count_spin_->setRange(1, 8);
  worker_count_spin_->setSingleStep(1);
  worker_count_spin_->setSuffix(" 个进程");
  form_perf->addRow("并发解析 Worker 进程数:", worker_count_spin_);

  min_img_chars_spin_ = new QSpinBox(tab_perf);
  min_img_chars_spin_->setRange(0, 50);
  min_img_chars_spin_->setSingleStep(1);
  min_img_chars_spin_->setSuffix(" 个字 (0为不限制)");
  form_perf->addRow("图片最少有效文字数（过滤表情/图标）:", min_img_chars_spin_);

  filter_thumbs_check_ = new QCheckBox("自动忽略应用缓存缩略图与图标 (如 WeAppIcon, Thumb)", tab_perf);
  form_perf->addRow("", filter_thumbs_check_);

  log_level_combo_ = new QComboBox(tab_perf);
  log_level_combo_->addItem("DEBUG (详细调试与耗时跟踪)", "debug");
  log_level_combo_->addItem("INFO  (标准运行信息，默认)", "info");
  log_level_combo_->addItem("WARN  (生产精简模式，仅警告/错误)", "warn");
  log_level_combo_->addItem("ERROR (仅严重故障记录)", "error");
  form_perf->addRow("运行日志详细级别:", log_level_combo_);

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
  form_perf->addRow("日志输出文件路径:", dt_path_layout);

  connect(dt_browse_btn_, &QPushButton::clicked, this, &SettingsDialog::OnBrowseDocToTextLogPath);

  tab_widget_->addTab(tab_perf, "性能与日志");

  main_layout->addWidget(tab_widget_);

  // -------------------------------------------------------------
  // Bottom Buttons: 重建索引 / 保存 / 取消
  // -------------------------------------------------------------
  auto* bottom_layout = new QHBoxLayout();
  auto* btn_rebuild = new QPushButton("清空并重建索引...", this);
  btn_rebuild->setStyleSheet("QPushButton { color: #D32F2F; font-weight: bold; }");
  bottom_layout->addWidget(btn_rebuild);
  bottom_layout->addStretch();
  auto* btn_save = new QPushButton("保存并应用", this);
  btn_save->setDefault(true);
  auto* btn_cancel = new QPushButton("取消", this);
  bottom_layout->addWidget(btn_save);
  bottom_layout->addWidget(btn_cancel);
  main_layout->addLayout(bottom_layout);

  connect(btn_rebuild, &QPushButton::clicked, this, &SettingsDialog::OnRebuildIndex);
  connect(btn_save, &QPushButton::clicked, this, &SettingsDialog::OnSaveAndApply);
  connect(btn_cancel, &QPushButton::clicked, this, &QDialog::reject);
}

void SettingsDialog::OnRebuildIndex() {
  auto reply = QMessageBox::warning(
      this, "确认重建索引",
      "此操作将彻底清空当前所有的全文倒排索引与密态缓存，\n"
      "并对所有配置的监控目录从头进行全量重新解析与构建。\n\n"
      "确定要继续吗？",
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

  dirs_list_->clear();
  for (const auto& dir : config_->GetIndexedDirectories()) {
    dirs_list_->addItem(QString::fromStdString(dir));
  }

  patterns_list_->clear();
  for (const auto& pat : config_->GetExcludedPatterns()) {
    patterns_list_->addItem(QString::fromStdString(pat));
  }

  exts_list_->clear();
  std::vector<std::string> sorted_exts;
  for (const auto& ext : config_->GetSupportedExtensions()) {
    sorted_exts.push_back(ext);
  }
  std::sort(sorted_exts.begin(), sorted_exts.end());
  for (const auto& ext : sorted_exts) {
    exts_list_->addItem(QString::fromStdString(ext));
  }

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
      this, "选择待索引的目录", QDir::homePath(),
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

void SettingsDialog::OnAddExtension() {
  QString ext = new_ext_edit_->text().trimmed().toLower();
  if (ext.startsWith('.')) {
    ext = ext.mid(1);
  }
  if (!ext.isEmpty()) {
    // Check duplicate
    for (int i = 0; i < exts_list_->count(); ++i) {
      if (exts_list_->item(i)->text() == ext) {
        return;
      }
    }
    exts_list_->addItem(ext);
    new_ext_edit_->clear();
  }
}

void SettingsDialog::OnRemoveExtension() {
  int row = exts_list_->currentRow();
  if (row >= 0) {
    delete exts_list_->takeItem(row);
  }
}

void SettingsDialog::OnResetExtensionsToDefault() {
  exts_list_->clear();
  std::vector<std::string> sorted_exts;
  for (const auto& ext : service::IndexConfig::GetDefaultSupportedExtensions()) {
    sorted_exts.push_back(ext);
  }
  std::sort(sorted_exts.begin(), sorted_exts.end());
  for (const auto& ext : sorted_exts) {
    exts_list_->addItem(QString::fromStdString(ext));
  }
}

void SettingsDialog::OnBrowseDocToTextLogPath() {
  QString cur_path = doctotext_log_path_edit_->text().trimmed();
  if (cur_path.isEmpty()) {
    cur_path = QDir::homePath() + "/doctotext.log";
  }
  QString file = QFileDialog::getSaveFileName(
      this, "选择 DocToText 日志输出文件", cur_path,
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

  // Save extensions
  std::vector<std::string> exts;
  for (int i = 0; i < exts_list_->count(); ++i) {
    exts.push_back(exts_list_->item(i)->text().toStdString());
  }
  config_->SetSupportedExtensions(exts);

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
