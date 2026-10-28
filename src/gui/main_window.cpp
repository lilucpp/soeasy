#include "gui/main_window.h"
#include "gui/icon_provider.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>
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
  setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
  setWindowTitle("SoEasy - 让电脑没有难搜的文件");
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
      "  background-color: #EEF4F9;"
      "  border: 1px solid #CBD5E1;"
      "}"
      "#centralWidget {"
      "  background-color: #EEF4F9;"
      "}"
      "QStatusBar {"
      "  background-color: #EEF4F9;"
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
  splitter_->setHandleWidth(5);
  splitter_->setStyleSheet(
      "QSplitter::handle {"
      "  background-color: #CBD5E1;"
      "}"
      "QSplitter::handle:hover {"
      "  background-color: #38BDF8;"
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
  status_stats_label_->setStyleSheet("color: #334155; font-size: 12px;");

  status_progress_bar_ = new QProgressBar(this);
  status_progress_bar_->setRange(0, 100);
  status_progress_bar_->setValue(0);
  status_progress_bar_->setTextVisible(true);
  status_progress_bar_->setFixedHeight(14);
  status_progress_bar_->setFixedWidth(130);
  status_progress_bar_->setStyleSheet(
      "QProgressBar {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 4px;"
      "  background-color: #E2E8F0;"
      "  text-align: center;"
      "  font-size: 10px;"
      "  font-weight: bold;"
      "  color: #1E293B;"
      "}"
      "QProgressBar::chunk {"
      "  background-color: #0EA5E9;"
      "  border-radius: 3px;"
      "}");
  status_progress_bar_->setVisible(false);

  status_file_label_ = new QLabel(this);
  status_file_label_->setStyleSheet("color: #64748B; font-size: 11px;");
  status_file_label_->setVisible(false);

  status_latency_label_ = new QLabel(this);
  status_latency_label_->setStyleSheet("color: #334155; font-size: 12px; font-weight: 500;");
  status_latency_label_->setTextFormat(Qt::RichText);

  statusBar()->addWidget(status_stats_label_);
  statusBar()->addWidget(status_progress_bar_);
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
}

void MainWindow::UpdateStatusBar(int match_count, double cost_ms) {
  size_t indexed_files = 0;
  if (pipeline_ && pipeline_->name_index()) {
    indexed_files = pipeline_->name_index()->size();
  }

  status_stats_label_->setText(
      QString("在索引文件: %1 个 | 当前匹配结果: %2 条")
          .arg(indexed_files)
          .arg(match_count));

  if (cost_ms > 0.0) {
    status_latency_label_->setText(
        QString("<span style=\"color: #4CAF50; font-size: 13px;\">●</span> 检索耗时: %1 ms")
            .arg(QString::number(cost_ms, 'f', 2)));
  } else {
    status_latency_label_->setText(
        "<span style=\"color: #4CAF50; font-size: 13px;\">●</span> 就绪");
  }
}

void MainWindow::OnSearchTriggered(const QString& query, int mode,
                                   const QString& path_scope) {
  current_query_ = query;

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

  if (mode == kModeFulltext) {
    // 1. Full-text inverted search (asynchronous non-blocking search on UI thread)
    auto hits = pipeline_->SearchFulltext(query_std, 100, 0, scope_std, /*sync_flush=*/false);
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
    auto name_hits = pipeline_->SearchFilename(query_std, 100, scope_std);
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
      item.snippet_html = QString("文件名匹配: %1").arg(QString::fromStdString(name_snippet));
      display_items.push_back(std::move(item));
    }
  }

  auto t1 = std::chrono::high_resolution_clock::now();
  double cost_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();

  result_view_->SetResults(display_items);
  if (!display_items.empty()) {
    result_view_->selectRow(0);
  } else if (preview_panel_) {
    preview_panel_->ClearPreview();
  }
  UpdateStatusBar(static_cast<int>(display_items.size()), cost_ms);
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
  if (pipeline_ && pipeline_->config()) {
    common::Logger::SetLevel(pipeline_->config()->log_level());
    common::Logger::SetConsoleEnabled(pipeline_->config()->log_console());
    common::Logger::SetSyncMode(pipeline_->config()->log_sync());
    if (pipeline_->worker_guard()) {
      pipeline_->worker_guard()->SetLoggingOptions(
          pipeline_->config()->log_level(),
          pipeline_->config()->log_console(),
          pipeline_->config()->log_sync());
    }
  }

  // Clear any temporary statusBar message so progress bar is visible immediately
  statusBar()->clearMessage();

  // Trigger background indexing for newly added or modified directories
  StartBackgroundInitialIndex();

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

  auto* progress = new QProgressDialog("正在准备全盘文件全文索引...", "取消", 0, 100, this);
  progress->setWindowTitle("重建索引");
  progress->setWindowModality(Qt::WindowModal);
  progress->setMinimumDuration(0);
  progress->setValue(0);
  progress->setAttribute(Qt::WA_DeleteOnClose);

  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  connect(progress, &QProgressDialog::canceled, [cancelled]() {
    cancelled->store(true);
  });

  std::thread worker_thread([this, progress, cancelled]() {
    bool ok = pipeline_->RebuildIndex(
        [this, progress, cancelled](size_t indexed, size_t total, const std::string& path) {
          if (cancelled->load()) {
            return;
          }
          if (total > 0) {
            int pct = static_cast<int>((indexed * 100) / total);
            QString label = QString("正在解析并建立索引 (%1/%2):\n%3")
                                .arg(indexed)
                                .arg(total)
                                .arg(QString::fromStdString(path));
            QMetaObject::invokeMethod(
                progress,
                [progress, pct, label]() {
                  progress->setValue(pct);
                  progress->setLabelText(label);
                },
                Qt::QueuedConnection);
          }
        });

    QMetaObject::invokeMethod(
        this,
        [this, progress, ok, cancelled]() {
          progress->setValue(100);
          progress->close();
          UpdateStatusBar(0, 0.0);

          if (cancelled->load()) {
            QMessageBox::information(this, "重建索引已取消", "索引重建过程已被用户取消。");
          } else if (ok) {
            QMessageBox::information(this, "重建索引完成",
                                     "所有配置目录的倒排全文索引与文件名秒索引已彻底重构完毕！");
            OnSearchTriggered(search_bar_->query(), search_bar_->mode(),
                              search_bar_->path_scope());
          } else {
            QMessageBox::critical(this, "重建索引失败",
                                  "重建索引过程中发生异常，请检查日志文件获取详细信息。");
          }
        },
        Qt::QueuedConnection);
  });

  worker_thread.detach();
}

void MainWindow::StartBackgroundInitialIndex() {
  if (is_indexing_.load()) {
    pending_reindex_.store(true);
    if (pipeline_) {
      pipeline_->CancelScan();
    }
    return;
  }
  is_indexing_.store(true);

  status_progress_bar_->setValue(0);
  status_progress_bar_->setVisible(true);
  status_file_label_->setText("正在扫描待索引目录...");
  status_file_label_->setVisible(true);
  status_stats_label_->setText("正在初始化索引...");

  if (background_index_thread_.joinable()) {
    background_index_thread_.join();
  }

  background_index_thread_ = std::thread([this]() {
    if (!pipeline_) {
      is_indexing_.store(false);
      emit IndexFinished(0);
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

    // 2. Perform initial directory scan in background worker thread
    size_t count = 0;
    if (!pipeline_->is_scan_cancelled()) {
      count = pipeline_->PerformInitialScan(
          [this](size_t indexed, size_t total, const std::string& path) {
            emit IndexProgressUpdated(static_cast<int>(indexed), static_cast<int>(total),
                                      QString::fromStdString(path));
          });
    }

    is_indexing_.store(false);
    emit IndexFinished(static_cast<int>(count));
  });
}

void MainWindow::OnIndexProgressUpdated(int indexed, int total, const QString& current_file) {
  if (total > 0) {
    int pct = (indexed * 100) / total;
    status_progress_bar_->setValue(pct);
    status_progress_bar_->setVisible(true);
    status_stats_label_->setText(
        QString("正在建立索引: %1/%2 (%3%)").arg(indexed).arg(total).arg(pct));
  } else {
    status_stats_label_->setText("正在扫描待索引目录...");
  }

  if (!current_file.isEmpty()) {
    status_file_label_->setVisible(true);
    QFontMetrics fm(status_file_label_->font());
    QString elided = fm.elidedText("正在解析: " + current_file, Qt::ElideMiddle, 500);
    status_file_label_->setText(elided);
    status_file_label_->setToolTip(current_file);
  }
}

void MainWindow::OnIndexFinished(int total_indexed) {
  Q_UNUSED(total_indexed);
  status_progress_bar_->setVisible(false);
  status_file_label_->setVisible(false);

  int match_count = result_view_ ? result_view_->rowCount() : 0;
  UpdateStatusBar(match_count, 0.0);

  // If another re-index was requested while indexing was active, start it now
  if (pending_reindex_.exchange(false)) {
    StartBackgroundInitialIndex();
    return;
  }

  // If search bar has active query, re-run search to surface newly indexed items immediately
  if (search_bar_ && !search_bar_->query().trimmed().isEmpty()) {
    OnSearchTriggered(search_bar_->query(), search_bar_->mode(), search_bar_->path_scope());
  }
}

}  // namespace gui
}  // namespace soeasy
