#include "gui/settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabBar>
#include <QTableWidget>
#include <QTextStream>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <string>
#include <vector>

#include "common/format_categories.h"
#include "common/logger.h"
#include "gui/i18n.h"
#include "storage/dat_jieba.h"

namespace soeasy {
namespace gui {

SettingsDialog::SettingsDialog(std::shared_ptr<service::IndexConfig> config, QWidget *parent)
    : QDialog(parent), config_(std::move(config)) {
  setObjectName("SettingsDialog");
  setWindowTitle("SoEasy - 索引配置与设置中心");
  setWindowIcon(QIcon(":/icons/app_icon.png"));
  resize(880, 620);
  SetupUi();
  PopulateFromConfig();
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::SetupUi() {
  auto *main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  // Body container: Left Navigation Sidebar + Right Content Area
  auto *body_widget = new QWidget(this);
  auto *body_layout = new QHBoxLayout(body_widget);
  body_layout->setContentsMargins(0, 0, 0, 0);
  body_layout->setSpacing(0);

  // 1. Left Nav Sidebar (Figma aligned with Back button & brand logo)
  auto *sidebar_widget = new QWidget(body_widget);
  sidebar_widget->setObjectName("SettingsSidebar");
  sidebar_widget->setFixedWidth(210);
  auto *sidebar_layout = new QVBoxLayout(sidebar_widget);
  sidebar_layout->setContentsMargins(12, 16, 12, 16);
  sidebar_layout->setSpacing(8);

  btn_back_ = new QPushButton("←  返回搜索", sidebar_widget);
  btn_back_->setObjectName("SettingsBackButton");
  btn_back_->setProperty("role", "secondary");
  btn_back_->setVisible(false);  // Shown only in embedded workspace mode
  connect(btn_back_, &QPushButton::clicked, this, &SettingsDialog::BackRequested);
  sidebar_layout->addWidget(btn_back_);

  auto *label_pref_title = new QLabel("偏好设置", sidebar_widget);
  label_pref_title->setProperty("role", "hint-label");
  sidebar_layout->addWidget(label_pref_title);

  nav_list_ = new QListWidget(sidebar_widget);
  nav_list_->setObjectName("SettingsNavList");
  nav_list_->setFocusPolicy(Qt::NoFocus);

  // Add 6 nav items
  new QListWidgetItem("常规与语言", nav_list_);
  new QListWidgetItem("索引目录", nav_list_);
  new QListWidgetItem("排除目录规则", nav_list_);
  new QListWidgetItem("文件格式与解析", nav_list_);
  new QListWidgetItem("分词词库", nav_list_);
  new QListWidgetItem("性能与日志", nav_list_);

  sidebar_layout->addWidget(nav_list_, 1);
  body_layout->addWidget(sidebar_widget);

  // 2. Right Content TabWidget
  tab_widget_ = new QTabWidget(body_widget);
  tab_widget_->setObjectName("SettingsTabWidget");
  if (tab_widget_->tabBar()) {
    tab_widget_->tabBar()->hide();
  }
  tab_widget_->setElideMode(Qt::ElideNone);

  tab_widget_->addTab(CreateGeneralTab(), "常规与语言");
  tab_widget_->addTab(CreateDirectoriesTab(), "索引目录");
  tab_widget_->addTab(CreatePatternsTab(), "排除目录规则");
  tab_widget_->addTab(CreateExtensionsTab(), "文件格式与解析");
  tab_widget_->addTab(CreateUserDictTab(), "分词词库");
  tab_widget_->addTab(CreatePerformanceTab(), "性能与日志");

  body_layout->addWidget(tab_widget_, 1);
  main_layout->addWidget(body_widget, 1);

  // Sync nav_list_ and tab_widget_
  connect(nav_list_, &QListWidget::currentRowChanged, tab_widget_, &QTabWidget::setCurrentIndex);
  connect(tab_widget_, &QTabWidget::currentChanged, this, [this](int index) {
    if (nav_list_ && nav_list_->currentRow() != index) {
      nav_list_->setCurrentRow(index);
    }
  });
  nav_list_->setCurrentRow(0);

  // 3. Bottom Action Bar
  auto *bottom_frame = new QFrame(this);
  bottom_frame->setObjectName("SettingsBottomBar");
  auto *bottom_layout = CreateBottomBar();
  bottom_frame->setLayout(bottom_layout);
  main_layout->addWidget(bottom_frame);

  RetranslateUi();
}

QWidget *SettingsDialog::CreateGeneralTab() {
  auto *tab_gen = new QWidget();
  tab_gen->setObjectName("SettingsTabPage");
  tab_gen->setAutoFillBackground(true);
  auto *vbox = new QVBoxLayout(tab_gen);
  vbox->setContentsMargins(24, 24, 24, 24);
  vbox->setSpacing(16);

  auto *card = new QFrame(tab_gen);
  card->setProperty("surface", "card");
  auto *form_gen = new QFormLayout(card);
  form_gen->setContentsMargins(20, 20, 20, 20);
  form_gen->setSpacing(14);

  label_language_ = new QLabel("界面显示语言 (UI Language):", card);
  language_combo_ = new QComboBox(card);
  language_combo_->addItem("简体中文 (Simplified Chinese)", "zh");
  language_combo_->addItem("English", "en");
  form_gen->addRow(label_language_, language_combo_);

  label_lang_hint_ = new QLabel(card);
  label_lang_hint_->setProperty("role", "hint-label");
  form_gen->addRow("", label_lang_hint_);

  vbox->addWidget(card);
  vbox->addStretch();

  connect(language_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
    std::string chosen_lang = language_combo_->currentData().toString().toStdString();
    I18n::Instance().SetLanguage(chosen_lang);
    RetranslateUi();
  });

  return tab_gen;
}

QWidget *SettingsDialog::CreateDirectoriesTab() {
  auto *tab_dirs = new QWidget();
  tab_dirs->setObjectName("SettingsTabPage");
  tab_dirs->setAutoFillBackground(true);
  auto *vbox = new QVBoxLayout(tab_dirs);
  vbox->setContentsMargins(24, 24, 24, 24);
  vbox->setSpacing(16);

  auto *card = new QFrame(tab_dirs);
  card->setProperty("surface", "card");
  auto *layout_dirs = new QVBoxLayout(card);
  layout_dirs->setContentsMargins(20, 20, 20, 20);
  layout_dirs->setSpacing(14);

  label_dirs_ = new QLabel("系统将实时监控并自动为以下目录及其子文件夹建立索引：", card);
  dirs_list_ = new QListWidget(card);

  auto *btn_layout_dirs = new QHBoxLayout();
  btn_add_dir_ = new QPushButton("添加目录...", card);
  btn_add_dir_->setProperty("role", "secondary");
  btn_rm_dir_ = new QPushButton("移除所选", card);
  btn_rm_dir_->setProperty("role", "secondary");
  btn_layout_dirs->addWidget(btn_add_dir_);
  btn_layout_dirs->addWidget(btn_rm_dir_);
  btn_layout_dirs->addStretch();

  layout_dirs->addWidget(label_dirs_);
  layout_dirs->addWidget(dirs_list_);
  layout_dirs->addLayout(btn_layout_dirs);

  vbox->addWidget(card);

  connect(btn_add_dir_, &QPushButton::clicked, this, &SettingsDialog::OnAddDirectory);
  connect(btn_rm_dir_, &QPushButton::clicked, this, &SettingsDialog::OnRemoveDirectory);

  return tab_dirs;
}

QWidget *SettingsDialog::CreatePatternsTab() {
  auto *tab_patterns = new QWidget();
  tab_patterns->setObjectName("SettingsTabPage");
  tab_patterns->setAutoFillBackground(true);
  auto *vbox = new QVBoxLayout(tab_patterns);
  vbox->setContentsMargins(24, 24, 24, 24);
  vbox->setSpacing(16);

  auto *card = new QFrame(tab_patterns);
  card->setProperty("surface", "card");
  auto *layout_patterns = new QVBoxLayout(card);
  layout_patterns->setContentsMargins(20, 20, 20, 20);
  layout_patterns->setSpacing(14);

  label_patterns_ = new QLabel("匹配以下路径规则的目录将被自动忽略（不建立倒排索引）：", card);
  patterns_list_ = new QListWidget(card);

  auto *add_pattern_layout = new QHBoxLayout();
  new_pattern_edit_ = new QLineEdit(card);
  new_pattern_edit_->setPlaceholderText("例如: /.git/ 或 /node_modules/");
  btn_add_pattern_ = new QPushButton("添加规则", card);
  btn_add_pattern_->setProperty("role", "secondary");
  btn_rm_pattern_ = new QPushButton("移除所选", card);
  btn_rm_pattern_->setProperty("role", "secondary");
  add_pattern_layout->addWidget(new_pattern_edit_);
  add_pattern_layout->addWidget(btn_add_pattern_);
  add_pattern_layout->addWidget(btn_rm_pattern_);

  layout_patterns->addWidget(label_patterns_);
  layout_patterns->addWidget(patterns_list_);
  layout_patterns->addLayout(add_pattern_layout);

  vbox->addWidget(card);

  connect(btn_add_pattern_, &QPushButton::clicked, this, &SettingsDialog::OnAddPattern);
  connect(btn_rm_pattern_, &QPushButton::clicked, this, &SettingsDialog::OnRemovePattern);

  return tab_patterns;
}

QWidget *SettingsDialog::CreateExtensionsTab() {
  auto *tab_exts = new QWidget();
  tab_exts->setObjectName("SettingsTabPage");
  tab_exts->setAutoFillBackground(true);
  auto *layout_exts = new QVBoxLayout(tab_exts);

  // Group 1: 嵌入图片解析选项
  group_ocr_ = new QGroupBox("高级解析能力", tab_exts);
  auto *layout_ocr = new QVBoxLayout(group_ocr_);
  embedded_image_ocr_check_ = new QCheckBox("提取并 OCR 解析文档内嵌图片 (Office 系列 / PDF / ODF 等)", group_ocr_);
  label_ocr_tip_ = new QLabel(
      "💡 默认不勾选。勾选后将提取 Office/PDF/ODF 等文档中的嵌入插图并调用 PP-OCRv6 引擎进行文本识别融合；\n"
      "    不勾选可获得极致解析性能（纯文本毫秒级完成索引）。",
      group_ocr_);
  label_ocr_tip_->setProperty("role", "hint-label");
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
  label_custom_ = new QLabel("添加自定义扩展名:", tab_exts);
  new_custom_ext_edit_ = new QLineEdit(tab_exts);
  new_custom_ext_edit_->setPlaceholderText("例如: vue 或 proto (无需加点)");
  btn_add_custom_ext_ = new QPushButton("添加格式", tab_exts);
  btn_add_custom_ext_->setProperty("role", "secondary");
  btn_rm_custom_ext_ = new QPushButton("移除所选自定义", tab_exts);
  btn_rm_custom_ext_->setProperty("role", "secondary");

  add_custom_layout->addWidget(label_custom_);
  add_custom_layout->addWidget(new_custom_ext_edit_);
  add_custom_layout->addWidget(btn_add_custom_ext_);
  add_custom_layout->addWidget(btn_rm_custom_ext_);
  layout_exts->addLayout(add_custom_layout);

  // Bottom action bar
  auto *bottom_exts_layout = new QHBoxLayout();
  btn_select_all_ = new QPushButton("全选", tab_exts);
  btn_select_all_->setProperty("role", "secondary");
  btn_deselect_all_ = new QPushButton("全不选", tab_exts);
  btn_deselect_all_->setProperty("role", "secondary");
  btn_reset_exts_ = new QPushButton("恢复推荐默认", tab_exts);
  btn_reset_exts_->setProperty("role", "secondary");
  exts_count_label_ = new QLabel("已启用: 0 种文件格式", tab_exts);
  exts_count_label_->setProperty("role", "count-label");

  bottom_exts_layout->addWidget(btn_select_all_);
  bottom_exts_layout->addWidget(btn_deselect_all_);
  bottom_exts_layout->addWidget(btn_reset_exts_);
  bottom_exts_layout->addStretch();
  bottom_exts_layout->addWidget(exts_count_label_);
  layout_exts->addLayout(bottom_exts_layout);

  connect(btn_add_custom_ext_, &QPushButton::clicked, this, &SettingsDialog::OnAddCustomExtension);
  connect(btn_rm_custom_ext_, &QPushButton::clicked, this, &SettingsDialog::OnRemoveCustomExtension);
  connect(btn_select_all_, &QPushButton::clicked, this, &SettingsDialog::OnSelectAllExtensions);
  connect(btn_deselect_all_, &QPushButton::clicked, this, &SettingsDialog::OnDeselectAllExtensions);
  connect(btn_reset_exts_, &QPushButton::clicked, this, &SettingsDialog::OnResetExtensionsToDefault);
  connect(exts_tree_, &QTreeWidget::itemChanged, this, &SettingsDialog::OnTreeItemChanged);

  return tab_exts;
}

QWidget *SettingsDialog::CreateUserDictTab() {
  auto *tab_dict = new QWidget();
  tab_dict->setObjectName("SettingsTabPage");
  tab_dict->setAutoFillBackground(true);
  auto *layout_dict = new QVBoxLayout(tab_dict);
  layout_dict->setContentsMargins(16, 16, 16, 16);
  layout_dict->setSpacing(12);

  label_user_dict_hint_ = new QLabel(tab_dict);
  label_user_dict_hint_->setWordWrap(true);
  layout_dict->addWidget(label_user_dict_hint_);

  user_dict_table_ = new QTableWidget(tab_dict);
  user_dict_table_->setColumnCount(1);
  user_dict_table_->setHorizontalHeaderLabels({Tr("col_user_dict_word")});
  user_dict_table_->horizontalHeader()->setStretchLastSection(true);
  user_dict_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  user_dict_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  user_dict_table_->setAlternatingRowColors(true);
  layout_dict->addWidget(user_dict_table_);

  // Minimalist input bar for adding custom terms
  auto *input_layout = new QHBoxLayout();
  user_word_input_ = new QLineEdit(tab_dict);
  user_word_input_->setPlaceholderText(Tr("word_ph"));

  btn_add_user_word_ = new QPushButton(tab_dict);
  btn_add_user_word_->setProperty("role", "secondary");
  btn_rm_user_word_ = new QPushButton(tab_dict);
  btn_rm_user_word_->setProperty("role", "secondary");

  input_layout->addWidget(user_word_input_, 1);
  input_layout->addWidget(btn_add_user_word_);
  input_layout->addWidget(btn_rm_user_word_);
  layout_dict->addLayout(input_layout);

  auto *bottom_layout = new QHBoxLayout();
  user_dict_count_label_ = new QLabel(tab_dict);
  user_dict_count_label_->setProperty("role", "hint-label");
  bottom_layout->addStretch();
  bottom_layout->addWidget(user_dict_count_label_);
  layout_dict->addLayout(bottom_layout);

  connect(btn_add_user_word_, &QPushButton::clicked, this, &SettingsDialog::OnAddUserWord);
  connect(user_word_input_, &QLineEdit::returnPressed, this, &SettingsDialog::OnAddUserWord);
  connect(btn_rm_user_word_, &QPushButton::clicked, this, &SettingsDialog::OnRemoveUserWord);

  // Initialize Jieba engine instance for instant word-existence probing
  QString dict_dir;
  if (config_ && !config_->dict_dir().empty()) {
    dict_dir = QString::fromStdString(config_->dict_dir());
  } else {
    dict_dir = QCoreApplication::applicationDirPath() + "/dict";
  }
  if (!jieba_) {
    jieba_ = std::make_unique<storage::DatJieba>();
    if (!jieba_->Init(dict_dir.toStdString())) {
      // Fallback search in standard paths
      QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/dict";
      jieba_->Init(fallback.toStdString());
    }
  }

  LoadUserDict();

  return tab_dict;
}

void SettingsDialog::LoadUserDict() {
  if (!user_dict_table_) return;
  user_dict_table_->setRowCount(0);

  QString dict_dir;
  if (config_ && !config_->dict_dir().empty()) {
    dict_dir = QString::fromStdString(config_->dict_dir());
  } else {
    dict_dir = QCoreApplication::applicationDirPath() + "/dict";
  }

  QString user_dict_file = dict_dir + "/user.dict.utf8";
  QFile file(user_dict_file);
  if (!file.exists()) {
    QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/dict/user.dict.utf8";
    if (QFile::exists(fallback)) {
      user_dict_file = fallback;
      file.setFileName(user_dict_file);
    }
  }

  if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QTextStream in(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    in.setCodec("UTF-8");
#endif
    while (!in.atEnd()) {
      QString line = in.readLine().trimmed();
      if (line.isEmpty() || line.startsWith("#")) continue;
      QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
      if (parts.isEmpty()) continue;
      QString word = parts[0];
      int freq = parts.size() > 1 ? parts[1].toInt() : 10000;
      if (freq <= 0) freq = 10000;

      int row = user_dict_table_->rowCount();
      user_dict_table_->insertRow(row);
      auto *item = new QTableWidgetItem(word);
      item->setData(Qt::UserRole, freq);
      user_dict_table_->setItem(row, 0, item);
    }
    file.close();
  }

  if (user_dict_count_label_) {
    user_dict_count_label_->setText(Tr("label_word_count").arg(user_dict_table_->rowCount()));
  }
}

void SettingsDialog::SaveUserDict() {
  if (!user_dict_table_) return;

  QString dict_dir;
  if (config_ && !config_->dict_dir().empty()) {
    dict_dir = QString::fromStdString(config_->dict_dir());
  } else {
    dict_dir = QCoreApplication::applicationDirPath() + "/dict";
  }

  QDir().mkpath(dict_dir);
  QString user_dict_file = dict_dir + "/user.dict.utf8";
  QFile file(user_dict_file);
  if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    QTextStream out(&file);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    out.setCodec("UTF-8");
#endif
    for (int r = 0; r < user_dict_table_->rowCount(); ++r) {
      auto *w_item = user_dict_table_->item(r, 0);
      if (w_item && !w_item->text().trimmed().isEmpty()) {
        QString w = w_item->text().trimmed();
        int f = w_item->data(Qt::UserRole).toInt();
        if (f <= 0) f = 10000;
        out << w << " " << f << "\n";
      }
    }
    file.close();
    LOG_INFO("[SettingsDialog] Successfully persisted {} custom user words to '{}'",
             user_dict_table_->rowCount(), user_dict_file.toStdString());
  }
}

void SettingsDialog::OnAddUserWord() {
  if (!user_word_input_ || !user_dict_table_) return;
  QString word = user_word_input_->text().trimmed();
  if (word.isEmpty()) return;

  // 1. Check duplicate in current table
  for (int r = 0; r < user_dict_table_->rowCount(); ++r) {
    auto *item = user_dict_table_->item(r, 0);
    if (item && item->text().compare(word, Qt::CaseInsensitive) == 0) {
      QMessageBox::information(this, Tr("tab_user_dict"), Tr("tip_word_exists_in_custom"));
      user_dict_table_->selectRow(r);
      user_dict_table_->scrollToItem(item);
      return;
    }
  }

  // 2. Nanosecond check if already recognized by Jieba base/overlay dictionary
  if (jieba_ && jieba_->HasWord(word.toStdString())) {
    QMessageBox::information(this, Tr("tab_user_dict"), Tr("tip_word_exists_in_system"));
    return;
  }

  // 3. Add to table with default 10000 weight
  int row = user_dict_table_->rowCount();
  user_dict_table_->insertRow(row);
  auto *item = new QTableWidgetItem(word);
  item->setData(Qt::UserRole, 10000);
  user_dict_table_->setItem(row, 0, item);

  if (jieba_) {
    jieba_->InsertUserWord(word.toStdString(), 10000);
  }

  user_word_input_->clear();
  user_dict_table_->scrollToBottom();
  if (user_dict_count_label_) {
    user_dict_count_label_->setText(Tr("label_word_count").arg(user_dict_table_->rowCount()));
  }
}

void SettingsDialog::OnRemoveUserWord() {
  if (!user_dict_table_) return;
  auto selected = user_dict_table_->selectionModel()->selectedRows();
  if (selected.isEmpty()) return;

  // Best practice: Delete rows in descending index order to avoid shifting indices
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

QWidget *SettingsDialog::CreatePerformanceTab() {
  auto *tab_perf = new QWidget();
  tab_perf->setObjectName("SettingsTabPage");
  tab_perf->setAutoFillBackground(true);
  auto *main_vbox = new QVBoxLayout(tab_perf);
  main_vbox->setContentsMargins(0, 0, 0, 0);

  auto *scroll = new QScrollArea(tab_perf);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setObjectName("SettingsScrollArea");

  auto *content_widget = new QWidget(scroll);
  content_widget->setObjectName("SettingsTabPage");
  content_widget->setAutoFillBackground(true);
  auto *vbox = new QVBoxLayout(content_widget);
  vbox->setContentsMargins(24, 24, 24, 24);
  vbox->setSpacing(16);

  // 1. Performance & Logging Options Card
  auto *card = new QFrame(content_widget);
  card->setProperty("surface", "card");
  auto *form_perf = new QFormLayout(card);
  form_perf->setContentsMargins(20, 20, 20, 20);
  form_perf->setSpacing(14);

  debounce_spin_ = new QSpinBox(card);
  debounce_spin_->setRange(50, 5000);
  debounce_spin_->setSingleStep(50);
  debounce_spin_->setSuffix(" 毫秒");
  label_debounce_ = new QLabel("文件变动去抖动等待延迟:", card);
  form_perf->addRow(label_debounce_, debounce_spin_);

  max_size_spin_ = new QSpinBox(card);
  max_size_spin_->setRange(1, 1024);
  max_size_spin_->setSingleStep(10);
  max_size_spin_->setSuffix(" MB");
  label_max_size_ = new QLabel("单文件最大全文索引上限:", card);
  form_perf->addRow(label_max_size_, max_size_spin_);

  worker_count_spin_ = new QSpinBox(card);
  worker_count_spin_->setRange(1, 8);
  worker_count_spin_->setSingleStep(1);
  worker_count_spin_->setSuffix(" 个进程");
  label_worker_count_ = new QLabel("并发解析 Worker 进程数:", card);
  form_perf->addRow(label_worker_count_, worker_count_spin_);

  min_img_chars_spin_ = new QSpinBox(card);
  min_img_chars_spin_->setRange(0, 50);
  min_img_chars_spin_->setSingleStep(1);
  min_img_chars_spin_->setSuffix(" 个字 (0为不限制)");
  label_min_img_chars_ = new QLabel("图片最少有效文字数（过滤表情/图标）:", card);
  form_perf->addRow(label_min_img_chars_, min_img_chars_spin_);

  filter_thumbs_check_ = new QCheckBox("自动忽略应用缓存缩略图与图标 (如 WeAppIcon, Thumb)", card);
  form_perf->addRow("", filter_thumbs_check_);

  log_level_combo_ = new QComboBox(card);
  log_level_combo_->addItem("DEBUG (详细调试与耗时跟踪)", "debug");
  log_level_combo_->addItem("INFO  (标准运行信息，默认)", "info");
  log_level_combo_->addItem("WARN  (生产精简模式，仅警告/错误)", "warn");
  log_level_combo_->addItem("ERROR (仅严重故障记录)", "error");
  label_log_level_ = new QLabel("运行日志详细级别:", card);
  form_perf->addRow(label_log_level_, log_level_combo_);

  log_console_check_ = new QCheckBox("输出控制台实时镜像 (Console stdout，用于终端调试)", card);
  form_perf->addRow("终端输出调试:", log_console_check_);

  log_sync_check_ = new QCheckBox("同步即时落盘模式 (Sync Flush，每条日志即刻刷盘，排查异常卡死)", card);
  form_perf->addRow("落盘同步模式:", log_sync_check_);

  doctotext_log_check_ =
      new QCheckBox("启用 DocToText 详细文档解析日志 (默认关闭，用于排查 Office/PDF 解析问题)", card);
  form_perf->addRow("DocToText 日志:", doctotext_log_check_);

  auto *dt_path_layout = new QHBoxLayout();
  doctotext_log_path_edit_ = new QLineEdit(card);
  doctotext_log_path_edit_->setPlaceholderText("留空则默认输出至 logs/doctotext.log");
  dt_browse_btn_ = new QPushButton("浏览...", card);
  dt_browse_btn_->setProperty("role", "secondary");
  dt_browse_btn_->setFixedWidth(80);
  dt_path_layout->addWidget(doctotext_log_path_edit_);
  dt_path_layout->addWidget(dt_browse_btn_);
  label_dt_path_ = new QLabel("日志输出文件路径:", card);
  form_perf->addRow(label_dt_path_, dt_path_layout);

  vbox->addWidget(card);

  // 2. Danger Zone Card (Index Maintenance - Rebuild Index converged here)
  danger_card_ = new QFrame(content_widget);
  danger_card_->setProperty("surface", "danger-card");
  auto *danger_layout = new QVBoxLayout(danger_card_);
  danger_layout->setContentsMargins(18, 16, 18, 16);
  danger_layout->setSpacing(10);

  danger_title_label_ = new QLabel(Tr("danger_zone_title"), danger_card_);
  danger_title_label_->setProperty("role", "danger-title");
  danger_layout->addWidget(danger_title_label_);

  danger_desc_label_ = new QLabel(Tr("danger_zone_desc"), danger_card_);
  danger_desc_label_->setProperty("role", "danger-desc");
  danger_desc_label_->setWordWrap(true);
  danger_layout->addWidget(danger_desc_label_);

  btn_rebuild_ = new QPushButton("清空并重建索引...", danger_card_);
  btn_rebuild_->setProperty("role", "danger");
  btn_rebuild_->setFixedWidth(160);
  danger_layout->addWidget(btn_rebuild_, 0, Qt::AlignLeft);

  vbox->addWidget(danger_card_);
  vbox->addStretch();

  scroll->setWidget(content_widget);
  main_vbox->addWidget(scroll);

  connect(dt_browse_btn_, &QPushButton::clicked, this, &SettingsDialog::OnBrowseDocToTextLogPath);
  connect(btn_rebuild_, &QPushButton::clicked, this, &SettingsDialog::OnRebuildIndex);

  return tab_perf;
}

QHBoxLayout *SettingsDialog::CreateBottomBar() {
  auto *bottom_layout = new QHBoxLayout();
  bottom_layout->setContentsMargins(20, 10, 20, 10);
  bottom_layout->setSpacing(12);

  bottom_layout->addStretch();
  btn_save_ = new QPushButton("保存并应用", this);
  btn_save_->setProperty("role", "primary");
  btn_save_->setDefault(true);
  btn_cancel_ = new QPushButton("取消", this);
  btn_cancel_->setProperty("role", "secondary");
  bottom_layout->addWidget(btn_save_);
  bottom_layout->addWidget(btn_cancel_);

  connect(btn_save_, &QPushButton::clicked, this, &SettingsDialog::OnSaveAndApply);
  connect(btn_cancel_, &QPushButton::clicked, this, [this]() {
    if (config_) {
      I18n::Instance().SetLanguage(config_->ui_language());
    }
    emit BackRequested();
    reject();
  });

  return bottom_layout;
}

void SettingsDialog::RetranslateUi() {
  setWindowTitle(Tr("settings_title"));
  if (nav_list_) {
    if (nav_list_->count() > 0) nav_list_->item(0)->setText(Tr("tab_general"));
    if (nav_list_->count() > 1) nav_list_->item(1)->setText(Tr("tab_dirs"));
    if (nav_list_->count() > 2) nav_list_->item(2)->setText(Tr("tab_patterns"));
    if (nav_list_->count() > 3) nav_list_->item(3)->setText(Tr("tab_exts"));
    if (nav_list_->count() > 4) nav_list_->item(4)->setText(Tr("tab_user_dict"));
    if (nav_list_->count() > 5) nav_list_->item(5)->setText(Tr("tab_perf"));
  }
  if (tab_widget_) {
    tab_widget_->setTabText(0, Tr("tab_general"));
    tab_widget_->setTabText(1, Tr("tab_dirs"));
    tab_widget_->setTabText(2, Tr("tab_patterns"));
    tab_widget_->setTabText(3, Tr("tab_exts"));
    tab_widget_->setTabText(4, Tr("tab_user_dict"));
    tab_widget_->setTabText(5, Tr("tab_perf"));
  }
  if (danger_title_label_) danger_title_label_->setText(Tr("danger_zone_title"));
  if (danger_desc_label_) danger_desc_label_->setText(Tr("danger_zone_desc"));

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
      auto *cat_item = exts_tree_->topLevelItem(i);
      if (cat_item == custom_category_item_) {
        cat_item->setText(0, Tr("custom_category_name"));
        cat_item->setText(1, Tr("custom_category_desc"));
        for (int c = 0; c < cat_item->childCount(); ++c) {
          auto *child = cat_item->child(c);
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
          auto *fmt_item = cat_item->child(c);
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

  // Tab: User Dict (FEAT-002)
  if (label_user_dict_hint_) label_user_dict_hint_->setText(Tr("label_user_dict_hint"));
  if (user_word_input_) user_word_input_->setPlaceholderText(Tr("word_ph"));
  if (btn_add_user_word_) btn_add_user_word_->setText(Tr("btn_add_word"));
  if (btn_rm_user_word_) btn_rm_user_word_->setText(Tr("btn_rm_word"));
  if (user_dict_table_) {
    user_dict_table_->setHorizontalHeaderLabels({Tr("col_user_dict_word")});
  }
  if (user_dict_count_label_ && user_dict_table_) {
    user_dict_count_label_->setText(Tr("label_word_count").arg(user_dict_table_->rowCount()));
  }

  // Tab 4: Performance
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
  auto reply = QMessageBox::warning(this, Tr("rebuild_confirm_title"), Tr("rebuild_confirm_text"),
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
  for (const auto &dir : config_->GetIndexedDirectories()) {
    dirs_list_->addItem(QString::fromStdString(dir));
  }

  patterns_list_->clear();
  for (const auto &pat : config_->GetExcludedPatterns()) {
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
  for (const auto &c_ext : custom_exts) {
    auto *item = new QTreeWidgetItem(custom_category_item_);
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
    auto *cat_item = exts_tree_->topLevelItem(i);
    if (cat_item == custom_category_item_) continue;
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto *fmt_item = cat_item->child(j);
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
  QString dir = QFileDialog::getExistingDirectory(this, Tr("dlg_select_dir"), QDir::homePath(),
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

void SettingsDialog::OnTreeItemChanged(QTreeWidgetItem *item, int column) {
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
    auto *cat_item = exts_tree_->topLevelItem(i);
    if (!cat_item->isDisabled()) {
      cat_item->setCheckState(0, Qt::Checked);
    }
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

void SettingsDialog::OnDeselectAllExtensions() {
  is_updating_tree_ = true;
  for (int i = 0; i < exts_tree_->topLevelItemCount(); ++i) {
    auto *cat_item = exts_tree_->topLevelItem(i);
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
    auto *cat_item = exts_tree_->topLevelItem(i);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto *fmt_item = cat_item->child(j);
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
    auto *cat_item = exts_tree_->topLevelItem(i);
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
  auto *item = new QTreeWidgetItem(custom_category_item_);
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
  auto *current = exts_tree_->currentItem();
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
  QString file = QFileDialog::getSaveFileName(this, Tr("dlg_select_log"), cur_path,
                                              "Log Files (*.log);;Text Files (*.txt);;All Files (*)");
  if (!file.isEmpty()) {
    doctotext_log_path_edit_->setText(file);
  }
}

void SettingsDialog::SetEmbeddedMode(bool embedded) {
  if (btn_back_) {
    btn_back_->setVisible(embedded);
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
    auto *cat_item = exts_tree_->topLevelItem(i);
    bool is_custom = (cat_item == custom_category_item_);
    for (int j = 0; j < cat_item->childCount(); ++j) {
      auto *fmt_item = cat_item->child(j);
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
  SaveUserDict();

  emit SettingsApplied();
  emit BackRequested();
  accept();
}

}  // namespace gui
}  // namespace soeasy
