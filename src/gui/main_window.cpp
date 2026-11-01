#include "gui/main_window.h"
#include "gui/icon_provider.h"
#include "gui/i18n.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/logger.h"

namespace soeasy {
namespace gui {

namespace {

std::vector<std::string> SplitQueryKeywords(const std::string& query) {
  std::vector<std::string> kws;
  std::istringstream iss(query);
  std::string word;
  while (iss >> word) {
    if (word == "AND" || word == "OR" || word == "NOT") {
      continue;
    }
    // Strip quotes or wildcards
    while (!word.empty() && (word.front() == '"' || word.front() == '\'')) {
      word.erase(word.begin());
    }
    while (!word.empty() && (word.back() == '"' || word.back() == '\'' || word.back() == '*')) {
      word.pop_back();
    }
    if (!word.empty()) {
      kws.push_back(word);
    }
  }
  return kws;
}

}  // namespace

MainWindow::MainWindow(std::shared_ptr<service::IndexPipeline> pipeline,
                       QWidget* parent)
    : QMainWindow(parent), pipeline_(std::move(pipeline)) {
  if (pipeline_ && pipeline_->config()) {
    I18n::Instance().SetLanguage(pipeline_->config()->ui_language());
  }
  setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
  setWindowTitle(Tr("app_title"));
  setWindowIcon(IconProvider::GetAppIcon());
  resize(1280, 750);
  SetupUi();

  connect(this, &MainWindow::IndexProgressUpdated, this,
          &MainWindow::OnIndexProgressUpdated, Qt::QueuedConnection);
  connect(this, &MainWindow::IndexFinished, this,
          &MainWindow::OnIndexFinished, Qt::QueuedConnection);

  // Initial stats
  UpdateStatusBar(0, 0.0);
}

MainWindow::~MainWindow() {
  if (pipeline_) {
    pipeline_->CancelScan();
  }
  if (background_index_thread_.joinable()) {
    background_index_thread_.join();
  }
}

void MainWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange) {
    if (title_bar_) {
      title_bar_->UpdateMaximizeButton(isMaximized());
    }
  }
}

void MainWindow::showEvent(QShowEvent* event) {
  QMainWindow::showEvent(event);
  if (!splitter_initialized_ && splitter_) {
    int total_width = splitter_->width();
    if (total_width > 0) {
      splitter_->setSizes({total_width * 6 / 10, total_width * 4 / 10});
      splitter_initialized_ = true;
    }
  }
}

void MainWindow::SetupUi() {
  setStyleSheet(
      "QMainWindow {"
      "  background-color: #FFFAFA;"
      "  border: 1px solid #CBD5E1;"
      "}"
      "#centralWidget {"
      "  background-color: #FFFAFA;"
      "}"
      "QStatusBar {"
      "  background-color: #FFFAFA;"
      "  border-top: 1px solid #CBD5E1;"
      "  color: #334155;"
      "  font-size: 12px;"
      "  padding: 2px 8px;"
      "}"
      "QStatusBar::item {"
      "  border: none;"
      "}");

  auto* central_widget = new QWidget(this);
  central_widget->setObjectName("centralWidget");

  auto* root_layout = new QVBoxLayout(central_widget);
  root_layout->setContentsMargins(0, 0, 0, 0);
  root_layout->setSpacing(0);

  // 1. Custom Frameless Title Bar
  title_bar_ = new TitleBar(this);
  root_layout->addWidget(title_bar_);

  // 2. Main Content Container
  auto* content_widget = new QWidget(central_widget);
  auto* content_layout = new QVBoxLayout(content_widget);
  content_layout->setContentsMargins(12, 6, 12, 6);
  content_layout->setSpacing(8);

  // Search Bar
  search_bar_ = new SearchBar(content_widget);
  content_layout->addWidget(search_bar_);

  // Splitter: Result Table (Left) + Master-Detail Preview Panel (Right)
  splitter_ = new QSplitter(Qt::Horizontal, content_widget);
  splitter_->setHandleWidth(6);
  splitter_->setStyleSheet(
      "QSplitter::handle {"
      "  background-color: transparent;"
      "}"
      "QSplitter::handle:hover {"
      "  background-color: #38BDF8;"
      "  border-radius: 3px;"
      "}"
      "QSplitter::handle:pressed {"
      "  background-color: #0284C7;"
      "}");

  result_view_ = new ResultView(splitter_);
  preview_panel_ = new PreviewPanel(splitter_);
  if (pipeline_) {
    result_view_->SetStorageEngine(pipeline_->storage_engine());
    preview_panel_->SetStorageEngine(pipeline_->storage_engine());
  }

  splitter_->addWidget(result_view_);
  splitter_->addWidget(preview_panel_);
  splitter_->setCollapsible(0, false);
  splitter_->setCollapsible(1, true);
  splitter_->setStretchFactor(0, 6);
  splitter_->setStretchFactor(1, 4);
  splitter_->setSizes({768, 512});

  content_layout->addWidget(splitter_, 1);
  root_layout->addWidget(content_widget, 1);

  setCentralWidget(central_widget);

  // Status Bar
  status_stats_label_ = new QLabel(this);
  status_stats_label_->setTextFormat(Qt::RichText);
  status_stats_label_->setStyleSheet("color: #334155; font-size: 12px;");

  status_progress_bar_ = new QProgressBar(this);
  status_progress_bar_->setRange(0, 100);
  status_progress_bar_->setValue(0);
  status_progress_bar_->setTextVisible(true);
  status_progress_bar_->setFixedHeight(14);
  status_progress_bar_->setFixedWidth(140);
  status_progress_bar_->setStyleSheet(
      "QProgressBar {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #E2E8F0;"
      "  text-align: center;"
      "  font-size: 10px;"
      "  font-weight: bold;"
      "  color: #0F172A;"
      "}"
      "QProgressBar::chunk {"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0EA5E9, stop:1 #38BDF8);"
      "  border-radius: 3px;"
      "}");
  status_progress_bar_->setVisible(false);

  status_cancel_btn_ = new QPushButton("✕ 取消", this);
  status_cancel_btn_->setObjectName("statusCancelBtn");
  status_cancel_btn_->setToolTip("取消当前正在进行的索引任务");
  status_cancel_btn_->setCursor(Qt::PointingHandCursor);
  status_cancel_btn_->setStyleSheet(
      "QPushButton#statusCancelBtn {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 3px;"
      "  background-color: #FFFFFF;"
      "  color: #64748B;"
      "  font-size: 11px;"
      "  padding: 1px 7px;"
      "  font-weight: 500;"
      "}"
      "QPushButton#statusCancelBtn:hover {"
      "  background-color: #FEE2E2;"
      "  color: #DC2626;"
      "  border-color: #FCA5A5;"
      "}"
      "QPushButton#statusCancelBtn:pressed {"
      "  background-color: #FECACA;"
      "}");
  status_cancel_btn_->setVisible(false);
  connect(status_cancel_btn_, &QPushButton::clicked, this, [this]() {
    if (pipeline_) {
      pipeline_->CancelScan();
    }
    status_cancel_btn_->setEnabled(false);
    status_stats_label_->setText(Tr("status_cancelling"));
  });

  status_file_label_ = new QLabel(this);
  status_file_label_->setStyleSheet("color: #64748B; font-size: 11px;");
  status_file_label_->setVisible(false);

  status_latency_label_ = new QLabel(this);
  status_latency_label_->setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;");
  status_latency_label_->setTextFormat(Qt::RichText);

  statusBar()->addWidget(status_stats_label_);
  statusBar()->addWidget(status_progress_bar_);
  statusBar()->addWidget(status_cancel_btn_);
  statusBar()->addWidget(status_file_label_, 1);
  statusBar()->addPermanentWidget(status_latency_label_);

  // Signal Connections
  connect(search_bar_, &SearchBar::SearchTriggered, this,
          &MainWindow::OnSearchTriggered);
  connect(search_bar_, &SearchBar::SettingsRequested, this,
          &MainWindow::OnOpenSettings);
  connect(search_bar_, &SearchBar::PreviewToggleRequested, this,
          &MainWindow::OnTogglePreview);
  connect(result_view_, &ResultView::FileSelected, this,
          &MainWindow::OnFileSelected);
  connect(result_view_, &ResultView::FileOpenRequested, this,
          &MainWindow::OnFileOpenRequested);
  connect(result_view_, &ResultView::LoadMoreRequested, this,
          &MainWindow::OnLoadMoreRequested);

  RetranslateUi();
}

void MainWindow::RetranslateUi() {
  setWindowTitle(Tr("app_title"));
  if (title_bar_) {
    title_bar_->RetranslateUi();
  }
  if (search_bar_) {
    search_bar_->RetranslateUi();
  }
  if (result_view_) {
    result_view_->RetranslateUi();
  }
  if (preview_panel_) {
    preview_panel_->RetranslateUi();
  }
  if (status_cancel_btn_) {
    status_cancel_btn_->setText(Tr("btn_cancel_index"));
    status_cancel_btn_->setToolTip(Tr("tip_cancel_index"));
  }
  UpdateStatusBar(total_matched_count_, last_search_cost_ms_);
}

void MainWindow::UpdateStatusBar(int total_matches, double cost_ms) {
  size_t indexed_files = 0;
  if (pipeline_ && pipeline_->name_index()) {
    indexed_files = pipeline_->name_index()->size();
  }

  status_stats_label_->setText(Tr("status_stats").arg(indexed_files).arg(total_matches));

  if (cost_ms > 0.0) {
    status_latency_label_->setText(Tr("status_latency").arg(QString::number(cost_ms, 'f', 2)));
  } else {
    status_latency_label_->setText(Tr("status_ready"));
  }
}

void MainWindow::OnSearchTriggered(const QString& query, int mode,
                                   const QString& path_scope) {
  current_query_ = query;
  current_search_mode_ = mode;
  current_search_path_scope_ = path_scope;
  current_search_offset_ = 0;
  total_matched_count_ = 0;
  last_search_cost_ms_ = 0.0;
  has_more_results_ = false;
  is_loading_more_ = false;

  if (query.isEmpty() && path_scope.isEmpty()) {
    result_view_->ClearResults();
    if (preview_panel_) {
      preview_panel_->ClearPreview();
    }
    UpdateStatusBar(0, 0.0);
    return;
  }

  if (!pipeline_) {
    return;
  }

  std::string query_std = query.toStdString();
  std::string scope_std = path_scope.toStdString();
  std::vector<DisplayResultItem> display_items;

  result_view_->SetCurrentQuery(query);
  auto t0 = std::chrono::high_resolution_clock::now();

  int total_count = 0;
  if (mode == kModeFulltext) {
    // 1. Full-text inverted search (asynchronous non-blocking search on UI thread)
    auto hits = pipeline_->SearchFulltext(query_std, kPageSize, 0, scope_std, /*sync_flush=*/false, &total_count);
    if (static_cast<int>(hits.size()) >= kPageSize && total_count > kPageSize) {
      has_more_results_ = true;
    }
    auto keywords = SplitQueryKeywords(query_std);

    for (const auto& hit : hits) {
      DisplayResultItem item;
      item.doc_id = hit.doc_id;
      item.file_name = QString::fromStdString(hit.file_name);
      item.file_path = QString::fromStdString(hit.file_path);
      item.ext = QString::fromStdString(hit.ext);
      item.file_size = hit.file_size;
      item.mtime = hit.mtime;
      item.score = hit.rank;

      // Extract snippet
      std::string snippet_raw = hit.snippet;
      item.has_body_snippet = hit.has_body_snippet;
      if (snippet_raw.empty()) {
        snippet_raw = storage::StorageEngine::GenerateSnippet(
            hit.file_name, keywords, 100);
      }
      if (snippet_raw.empty()) {
        snippet_raw = hit.file_name;
      }
      item.snippet_html = QString::fromStdString(snippet_raw);
      display_items.push_back(std::move(item));
    }
  } else {
    // 2. Instantaneous filename search
    auto name_hits = pipeline_->SearchFilename(query_std, kPageSize, 0, scope_std, &total_count);
    if (static_cast<int>(name_hits.size()) >= kPageSize && total_count > kPageSize) {
      has_more_results_ = true;
    }
    auto keywords = SplitQueryKeywords(query_std);

    for (const auto& nh : name_hits) {
      DisplayResultItem item;
      item.doc_id = nh.doc_id;
      item.file_name = QString::fromStdString(nh.file_name);
      item.file_path = QString::fromStdString(nh.file_path);
      item.ext = QString::fromStdString(nh.ext);
      item.file_size = nh.file_size;
      item.mtime = nh.mtime;
      item.score = nh.score;
      std::string name_snippet = storage::StorageEngine::GenerateSnippet(
          nh.file_name, keywords, 100);
      if (name_snippet.empty()) {
        name_snippet = nh.file_name;
      }
      item.snippet_html = Tr("filename_match_prefix").arg(QString::fromStdString(name_snippet));
      display_items.push_back(std::move(item));
    }
  }

  auto t1 = std::chrono::high_resolution_clock::now();
  double cost_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  total_matched_count_ = total_count;
  last_search_cost_ms_ = cost_ms;
  current_search_offset_ = static_cast<int>(display_items.size());
  result_view_->SetResults(display_items);
  if (!display_items.empty()) {
    result_view_->selectRow(0);
  } else if (preview_panel_) {
    preview_panel_->ClearPreview();
  }
  UpdateStatusBar(total_matched_count_, last_search_cost_ms_);
}

void MainWindow::OnLoadMoreRequested() {
  if (is_loading_more_ || !has_more_results_ || current_query_.trimmed().isEmpty() || !pipeline_) {
    return;
  }
  is_loading_more_ = true;

  std::string query_std = current_query_.toStdString();
  std::string scope_std = current_search_path_scope_.toStdString();
  std::vector<DisplayResultItem> new_display_items;

  auto t0 = std::chrono::high_resolution_clock::now();

  if (current_search_mode_ == kModeFulltext) {
    auto hits = pipeline_->SearchFulltext(query_std, kPageSize, current_search_offset_, scope_std, /*sync_flush=*/false);
    if (static_cast<int>(hits.size()) < kPageSize || current_search_offset_ + static_cast<int>(hits.size()) >= total_matched_count_) {
      has_more_results_ = false;
    }
    auto keywords = SplitQueryKeywords(query_std);

    for (const auto& hit : hits) {
      DisplayResultItem item;
      item.doc_id = hit.doc_id;
      item.file_name = QString::fromStdString(hit.file_name);
      item.file_path = QString::fromStdString(hit.file_path);
      item.ext = QString::fromStdString(hit.ext);
      item.file_size = hit.file_size;
      item.mtime = hit.mtime;
      item.score = hit.rank;

      std::string snippet_raw = hit.snippet;
      item.has_body_snippet = hit.has_body_snippet;
      if (snippet_raw.empty()) {
        snippet_raw = storage::StorageEngine::GenerateSnippet(
            hit.file_name, keywords, 100);
      }
      if (snippet_raw.empty()) {
        snippet_raw = hit.file_name;
      }
      item.snippet_html = QString::fromStdString(snippet_raw);
      new_display_items.push_back(std::move(item));
    }
  } else {
    auto name_hits = pipeline_->SearchFilename(query_std, kPageSize, current_search_offset_, scope_std);
    if (static_cast<int>(name_hits.size()) < kPageSize || current_search_offset_ + static_cast<int>(name_hits.size()) >= total_matched_count_) {
      has_more_results_ = false;
    }
    auto keywords = SplitQueryKeywords(query_std);

    for (const auto& nh : name_hits) {
      DisplayResultItem item;
      item.doc_id = nh.doc_id;
      item.file_name = QString::fromStdString(nh.file_name);
      item.file_path = QString::fromStdString(nh.file_path);
      item.ext = QString::fromStdString(nh.ext);
      item.file_size = nh.file_size;
      item.mtime = nh.mtime;
      item.score = nh.score;
      std::string name_snippet = storage::StorageEngine::GenerateSnippet(
          nh.file_name, keywords, 100);
      if (name_snippet.empty()) {
        name_snippet = nh.file_name;
      }
      item.snippet_html = Tr("filename_match_prefix").arg(QString::fromStdString(name_snippet));
      new_display_items.push_back(std::move(item));
    }
  }

  auto t1 = std::chrono::high_resolution_clock::now();
  double cost_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (!new_display_items.empty()) {
    current_search_offset_ += static_cast<int>(new_display_items.size());
    result_view_->AppendResults(new_display_items);
    last_search_cost_ms_ = cost_ms;
    UpdateStatusBar(total_matched_count_, last_search_cost_ms_);
  } else {
    has_more_results_ = false;
  }

  is_loading_more_ = false;
}

void MainWindow::OnFileSelected(int64_t doc_id, const QString& file_name,
                                const QString& file_path) {
  if (preview_panel_ && preview_panel_->isVisible()) {
    auto keywords = SplitQueryKeywords(current_query_.toStdString());
    preview_panel_->LoadDocument(doc_id, file_name, file_path, keywords);
  }
}

void MainWindow::OnTogglePreview() {
  if (preview_panel_) {
    preview_panel_->setVisible(!preview_panel_->isVisible());
  }
}

void MainWindow::OnFileOpenRequested(int64_t doc_id, const QString& file_name,
                                     const QString& file_path) {
  OnFileSelected(doc_id, file_name, file_path);

  // Modern desktop search standard: double click directly opens with OS default program
  bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(file_path));
  if (!opened) {
    auto keywords = SplitQueryKeywords(current_query_.toStdString());
    auto storage = pipeline_ ? pipeline_->storage_engine() : nullptr;
    ContentPreviewDialog dlg(doc_id, file_name, file_path, keywords, storage, this);
    dlg.exec();
  }
}

void MainWindow::OnOpenSettings() {
  if (!pipeline_ || !pipeline_->config()) {
    return;
  }
  SettingsDialog dlg(pipeline_->config(), this);
  connect(&dlg, &SettingsDialog::SettingsApplied, this,
          &MainWindow::OnSettingsApplied);
  connect(&dlg, &SettingsDialog::RebuildIndexRequested, this,
          &MainWindow::OnRebuildIndexRequested);
  dlg.exec();
}

void MainWindow::OnSettingsApplied() {
  RetranslateUi();

  if (pipeline_ && pipeline_->config()) {
    common::Logger::SetLevel(pipeline_->config()->log_level());
    common::Logger::SetConsoleEnabled(pipeline_->config()->log_console());
    common::Logger::SetSyncMode(pipeline_->config()->log_sync());
    if (pipeline_->worker_guard()) {
      pipeline_->worker_guard()->SetLoggingOptions(
          pipeline_->config()->log_level(),
          pipeline_->config()->log_console(),
          pipeline_->config()->log_sync(),
          pipeline_->config()->doctotext_logging(),
          pipeline_->config()->doctotext_log_path(),
          pipeline_->config()->enable_embedded_image_ocr());
    }
  }

  // Clear any temporary statusBar message so progress bar is visible immediately
  statusBar()->clearMessage();

  // Trigger background indexing for newly added or modified directories
  StartBackgroundInitialIndex(true);

  // Re-trigger current search only if there is an active search query
  if (search_bar_ && (!search_bar_->query().trimmed().isEmpty() ||
                      !search_bar_->path_scope().trimmed().isEmpty())) {
    OnSearchTriggered(search_bar_->query(), search_bar_->mode(),
                      search_bar_->path_scope());
  }
}

void MainWindow::OnRebuildIndexRequested() {
  if (!pipeline_) {
    return;
  }

  // Cancel any existing scan
  if (is_indexing_.load()) {
    pipeline_->CancelScan();
  }
  if (background_index_thread_.joinable()) {
    background_index_thread_.join();
  }

  is_indexing_.store(true);
  current_task_type_ = kTaskRebuild;
  index_start_time_ = std::chrono::steady_clock::now();

  statusBar()->clearMessage();
  status_stats_label_->setText(
      "<span style=\"background-color:#FEF3C7; color:#B45309; font-weight:bold; padding:1px 6px; border-radius:3px;\">重建索引</span> 正在清空旧缓存并扫描目录...");
  status_progress_bar_->setValue(0);
  status_progress_bar_->setVisible(true);
  status_cancel_btn_->setVisible(true);
  status_cancel_btn_->setEnabled(true);
  status_file_label_->setText("正在初始化文件清单...");
  status_file_label_->setVisible(true);
  status_latency_label_->setText("<span style=\"color:#D97706;\">●</span> 重建中");

  background_index_thread_ = std::thread([this]() {
    auto t0 = std::chrono::steady_clock::now();
    bool ok = pipeline_->RebuildIndex(
        [this](size_t indexed, size_t total, const std::string& path) {
          emit IndexProgressUpdated(kTaskRebuild, static_cast<int>(indexed),
                                    static_cast<int>(total), QString::fromStdString(path));
        });
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
    bool cancelled = pipeline_->is_scan_cancelled();
    is_indexing_.store(false);
    size_t count = (ok && pipeline_->name_index()) ? pipeline_->name_index()->size() : 0;
    emit IndexFinished(kTaskRebuild, static_cast<int>(count), cancelled, elapsed_sec);
  });
}

void MainWindow::StartBackgroundInitialIndex(bool force_incremental) {
  if (is_indexing_.load()) {
    pending_reindex_.store(true);
    if (pipeline_) {
      pipeline_->CancelScan();
    }
    return;
  }
  is_indexing_.store(true);

  bool is_cold = !force_incremental && (!pipeline_->name_index() || pipeline_->name_index()->size() == 0);
  int task_type = is_cold ? kTaskInitial : kTaskIncremental;
  current_task_type_ = task_type;
  index_start_time_ = std::chrono::steady_clock::now();

  QString badge = is_cold
      ? "<span style=\"background-color:#E0F2FE; color:#0369A1; font-weight:bold; padding:1px 6px; border-radius:3px;\">首次索引</span> "
      : "<span style=\"background-color:#DCFCE7; color:#15803D; font-weight:bold; padding:1px 6px; border-radius:3px;\">增量索引</span> ";

  status_stats_label_->setText(badge + "正在扫描待索引目录...");
  status_progress_bar_->setValue(0);
  status_progress_bar_->setVisible(true);
  status_cancel_btn_->setVisible(true);
  status_cancel_btn_->setEnabled(true);
  status_file_label_->setText("正在初始化文件清单...");
  status_file_label_->setVisible(true);
  status_latency_label_->setText(is_cold
      ? "<span style=\"color:#0284C7;\">●</span> 初始化中"
      : "<span style=\"color:#0D9488;\">●</span> 同步中");

  if (background_index_thread_.joinable()) {
    background_index_thread_.join();
  }

  background_index_thread_ = std::thread([this, task_type]() {
    auto t0 = std::chrono::steady_clock::now();

    if (!pipeline_) {
      is_indexing_.store(false);
      emit IndexFinished(task_type, 0, false, 0.0);
      return;
    }

    // 1. Synchronize watch engine directories in background worker thread (non-blocking for UI)
    if (pipeline_->watch_engine() && pipeline_->config()) {
      auto configured_dirs = pipeline_->config()->GetIndexedDirectories();
      auto watched_dirs = pipeline_->watch_engine()->GetWatchedDirectories();
      std::unordered_set<std::string> active_set(configured_dirs.begin(), configured_dirs.end());

      for (const auto& watched : watched_dirs) {
        if (pipeline_->is_scan_cancelled()) break;
        if (active_set.find(watched) == active_set.end()) {
          pipeline_->watch_engine()->RemoveWatch(watched);
        }
      }
      for (const auto& dir : configured_dirs) {
        if (pipeline_->is_scan_cancelled()) break;
        pipeline_->watch_engine()->AddWatch(dir);
      }
    }

    // 2. Perform directory scan in background worker thread
    size_t count = 0;
    if (!pipeline_->is_scan_cancelled()) {
      count = pipeline_->PerformInitialScan(
          [this, task_type](size_t indexed, size_t total, const std::string& path) {
            emit IndexProgressUpdated(task_type, static_cast<int>(indexed), static_cast<int>(total),
                                      QString::fromStdString(path));
          });
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();
    bool cancelled = pipeline_->is_scan_cancelled();
    is_indexing_.store(false);
    emit IndexFinished(task_type, static_cast<int>(count), cancelled, elapsed_sec);
  });
}

void MainWindow::OnIndexProgressUpdated(int task_type, int indexed, int total,
                                        const QString& current_file) {
  QString badge;
  if (task_type == kTaskRebuild) {
    badge = "<span style=\"background-color:#FEF3C7; color:#B45309; font-weight:bold; padding:1px 6px; border-radius:3px;\">重建索引</span> ";
  } else if (task_type == kTaskInitial) {
    badge = "<span style=\"background-color:#E0F2FE; color:#0369A1; font-weight:bold; padding:1px 6px; border-radius:3px;\">首次索引</span> ";
  } else {
    badge = "<span style=\"background-color:#DCFCE7; color:#15803D; font-weight:bold; padding:1px 6px; border-radius:3px;\">增量索引</span> ";
  }

  if (total > 0) {
    int pct = (indexed * 100) / total;
    status_progress_bar_->setValue(pct);
    status_progress_bar_->setVisible(true);
    status_stats_label_->setText(
        badge + QString("正在构建: %1/%2 (%3%)").arg(indexed).arg(total).arg(pct));
  } else {
    status_stats_label_->setText(badge + "正在扫描待索引目录...");
  }

  if (!current_file.isEmpty()) {
    status_file_label_->setVisible(true);
    QFontMetrics fm(status_file_label_->font());
    QString elided = fm.elidedText("正在解析: " + current_file, Qt::ElideMiddle, 460);
    status_file_label_->setText(elided);
    status_file_label_->setToolTip(current_file);
  }
}

void MainWindow::OnIndexFinished(int task_type, int total_indexed, bool cancelled,
                                 double elapsed_sec) {
  status_progress_bar_->setVisible(false);
  status_file_label_->setVisible(false);
  status_cancel_btn_->setVisible(false);

  // If another re-index was requested while indexing was active, start it now
  if (pending_reindex_.exchange(false)) {
    StartBackgroundInitialIndex(true);
    return;
  }

  if (cancelled) {
    status_stats_label_->setText(
        "<span style=\"color:#D97706; font-weight:bold;\">! 索引任务已取消</span> 已保留已解析的数据");
    status_latency_label_->setText("<span style=\"color:#16A34A;\">●</span> 就绪");
  } else {
    QString finish_desc;
    if (task_type == kTaskRebuild) {
      finish_desc = QString("<span style=\"color:#16A34A; font-weight:bold;\">✓ 索引重建完成</span> "
                            "共重构 %1 个文件索引 (耗时 %2s)")
                        .arg(total_indexed)
                        .arg(QString::number(elapsed_sec, 'f', 1));
    } else if (task_type == kTaskInitial) {
      finish_desc = QString("<span style=\"color:#16A34A; font-weight:bold;\">✓ 首次索引完成</span> "
                            "共索引 %1 个文件 (耗时 %2s)")
                        .arg(total_indexed)
                        .arg(QString::number(elapsed_sec, 'f', 1));
    } else {
      finish_desc = QString("<span style=\"color:#16A34A; font-weight:bold;\">✓ 增量同步完成</span> "
                            "索引已是最新 (耗时 %1s)")
                        .arg(QString::number(elapsed_sec, 'f', 1));
    }
    status_stats_label_->setText(finish_desc);
    status_latency_label_->setText("<span style=\"color:#16A34A;\">●</span> 就绪");
  }

  // Smoothly restore normal idle statistics after 3.5 seconds
  QTimer::singleShot(3500, this, [this]() {
    if (!is_indexing_.load()) {
      UpdateStatusBar(total_matched_count_, last_search_cost_ms_);
    }
  });

  // If search bar has active query, re-run search to surface newly indexed items immediately
  if (search_bar_ && !search_bar_->query().trimmed().isEmpty()) {
    OnSearchTriggered(search_bar_->query(), search_bar_->mode(), search_bar_->path_scope());
  }
}

}  // namespace gui
}  // namespace soeasy
