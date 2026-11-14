#include "gui/main_window.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

#include "common/logger.h"
#include "common/snippet_highlighter.h"
#include "gui/i18n.h"
#include "gui/icon_provider.h"

namespace soeasy {
namespace gui {

namespace {

std::vector<std::string> SplitQueryKeywords(const std::string &query) {
  return common::SnippetHighlighter::ExtractKeywordsFromQuery(query);
}

QString IndexTaskBadge(int task_type) {
  if (task_type == static_cast<int>(service::IndexTaskKind::kRebuild)) {
    return Tr("status_badge_rebuild");
  }
  if (task_type == static_cast<int>(service::IndexTaskKind::kInitial)) {
    return Tr("status_badge_initial");
  }
  return Tr("status_badge_incremental");
}

std::vector<DisplayResultItem> BuildFulltextDisplayItems(const std::vector<service::SearchResultItem> &hits,
                                                         const std::vector<std::string> &keywords) {
  std::vector<DisplayResultItem> items;
  items.reserve(hits.size());
  for (const auto &hit : hits) {
    DisplayResultItem item;
    item.doc_id = hit.doc_id;
    item.file_name = QString::fromStdString(hit.file_name);
    item.file_path = QString::fromStdString(hit.file_path);
    item.ext = QString::fromStdString(hit.ext);
    item.file_size = hit.file_size;
    item.mtime = hit.mtime;
    item.score = (hit.rank < 0) ? -hit.rank : hit.rank;

    std::string snippet_raw = hit.snippet;
    item.has_body_snippet = hit.has_body_snippet;
    if (snippet_raw.empty()) {
      snippet_raw = common::SnippetHighlighter::GenerateSnippet(hit.file_name, keywords, 100);
    }
    if (snippet_raw.empty()) {
      snippet_raw = hit.file_name;
    }
    item.snippet_html = QString::fromStdString(snippet_raw);
    items.push_back(std::move(item));
  }
  return items;
}

std::vector<DisplayResultItem> BuildFilenameDisplayItems(const std::vector<service::NameMatchResult> &name_hits,
                                                         const std::vector<std::string> &keywords) {
  std::vector<DisplayResultItem> items;
  items.reserve(name_hits.size());
  for (const auto &nh : name_hits) {
    DisplayResultItem item;
    item.doc_id = nh.doc_id;
    item.file_name = QString::fromStdString(nh.file_name);
    item.file_path = QString::fromStdString(nh.file_path);
    item.ext = QString::fromStdString(nh.ext);
    item.file_size = nh.file_size;
    item.mtime = nh.mtime;
    item.score = nh.score;
    std::string name_snippet = common::SnippetHighlighter::GenerateSnippet(nh.file_name, keywords, 100);
    if (name_snippet.empty()) {
      name_snippet = nh.file_name;
    }
    item.snippet_html = Tr("filename_match_prefix").arg(QString::fromStdString(name_snippet));
    items.push_back(std::move(item));
  }
  return items;
}

}  // namespace

MainWindow::MainWindow(std::shared_ptr<service::IndexPipeline> pipeline, QWidget *parent)
    : QMainWindow(parent), pipeline_(std::move(pipeline)), callback_target_(std::make_shared<CallbackTarget>()) {
  callback_target_->window = this;
  if (pipeline_ && pipeline_->config()) {
    I18n::Instance().SetLanguage(pipeline_->config()->ui_language());
  }
  setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
  setWindowTitle(Tr("app_title"));
  setWindowIcon(IconProvider::GetAppIcon());
  resize(1280, 750);
  SetupUi();
  SetupTrayIcon();

  connect(this, &MainWindow::IndexTaskStarted, this, &MainWindow::OnIndexTaskStarted, Qt::QueuedConnection);
  connect(this, &MainWindow::IndexProgressUpdated, this, &MainWindow::OnIndexProgressUpdated, Qt::QueuedConnection);
  connect(this, &MainWindow::IndexFinished, this, &MainWindow::OnIndexFinished, Qt::QueuedConnection);

  // Initial stats
  UpdateStatusBar(0, 0.0);
}

MainWindow::~MainWindow() {
  if (tray_icon_) {
    tray_icon_->hide();
  }
  if (pipeline_) {
    pipeline_->ClearBackgroundCallbacks();
    pipeline_->CancelScan();
  }
  if (callback_target_) {
    std::lock_guard<std::mutex> lock(callback_target_->mutex);
    callback_target_->window = nullptr;
  }
}

void MainWindow::changeEvent(QEvent *event) {
  QMainWindow::changeEvent(event);
  if (event->type() == QEvent::WindowStateChange) {
    if (title_bar_) {
      title_bar_->UpdateMaximizeButton(isMaximized());
    }
  }
}

void MainWindow::showEvent(QShowEvent *event) {
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
      "  font-family: 'Microsoft YaHei UI', 'Microsoft YaHei', 'Segoe UI', 'PingFang SC', 'Noto Sans CJK SC', "
      "'WenQuanYi Micro Hei', sans-serif;"
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

  auto *central_widget = new QWidget(this);
  central_widget->setObjectName("centralWidget");

  auto *root_layout = new QVBoxLayout(central_widget);
  root_layout->setContentsMargins(0, 0, 0, 0);
  root_layout->setSpacing(0);

  // 1. Custom Frameless Title Bar
  title_bar_ = new TitleBar(this);
  root_layout->addWidget(title_bar_);

  // 2. Main Content Container
  auto *content_widget = new QWidget(central_widget);
  auto *content_layout = new QVBoxLayout(content_widget);
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
    result_view_->SetSearchCoordinator(pipeline_->search_coordinator());
    preview_panel_->SetSearchCoordinator(pipeline_->search_coordinator());
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
      "  color: #0F172A;"
      "}"
      "QProgressBar::chunk {"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0EA5E9, stop:1 #38BDF8);"
      "  border-radius: 3px;"
      "}");
  status_progress_bar_->setVisible(false);

  status_cancel_btn_ = new QPushButton(Tr("btn_cancel_index"), this);
  status_cancel_btn_->setObjectName("statusCancelBtn");
  status_cancel_btn_->setToolTip(Tr("tip_cancel_index"));
  status_cancel_btn_->setCursor(Qt::PointingHandCursor);
  status_cancel_btn_->setStyleSheet(
      "QPushButton#statusCancelBtn {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 3px;"
      "  background-color: #FFFFFF;"
      "  color: #64748B;"
      "  font-size: 11px;"
      "  padding: 1px 7px;"
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
  status_latency_label_->setStyleSheet("color: #334155; font-size: 12px;");
  status_latency_label_->setTextFormat(Qt::RichText);

  status_preview_btn_ = new QPushButton(this);
  status_preview_btn_->setObjectName("statusPreviewBtn");
  status_preview_btn_->setCheckable(true);
  status_preview_btn_->setChecked(true);
  status_preview_btn_->setText(Tr("btn_preview"));
  status_preview_btn_->setToolTip(Tr("tip_preview"));
  status_preview_btn_->setCursor(Qt::PointingHandCursor);
  status_preview_btn_->setStyleSheet(
      "QPushButton#statusPreviewBtn {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 3px;"
      "  background-color: #FFFFFF;"
      "  color: #334155;"
      "  font-size: 11px;"
      "  padding: 1px 8px;"
      "}"
      "QPushButton#statusPreviewBtn:hover {"
      "  background-color: #F1F5F9;"
      "  border-color: #94A3B8;"
      "  color: #0F172A;"
      "}"
      "QPushButton#statusPreviewBtn:checked {"
      "  background-color: #E0F2FE;"
      "  color: #0284C7;"
      "  border-color: #BAE6FD;"
      "}");
  connect(status_preview_btn_, &QPushButton::clicked, this, &MainWindow::OnTogglePreview);

  statusBar()->addWidget(status_stats_label_);
  statusBar()->addWidget(status_progress_bar_);
  statusBar()->addWidget(status_cancel_btn_);
  statusBar()->addWidget(status_file_label_, 1);
  statusBar()->addPermanentWidget(status_preview_btn_);
  statusBar()->addPermanentWidget(status_latency_label_);

  // Signal Connections
  connect(search_bar_, &SearchBar::SearchTriggered, this, &MainWindow::OnSearchTriggered);
  connect(search_bar_, &SearchBar::SettingsRequested, this, &MainWindow::OnOpenSettings);
  connect(preview_panel_, &PreviewPanel::CloseRequested, this, &MainWindow::OnTogglePreview);
  connect(search_bar_, &SearchBar::NavigateResultsRequested, this, [this]() {
    if (result_view_ && result_view_->rowCount() > 0) {
      result_view_->setFocus();
      if (!result_view_->currentIndex().isValid()) {
        result_view_->selectRow(0);
      }
    }
  });
  connect(result_view_, &ResultView::FileSelected, this, &MainWindow::OnFileSelected);
  connect(result_view_, &ResultView::FileOpenRequested, this, &MainWindow::OnFileOpenRequested);
  connect(result_view_, &ResultView::LoadMoreRequested, this, &MainWindow::OnLoadMoreRequested);

  // Global Keyboard Shortcuts
  auto *sc_focus = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_F), this);
  connect(sc_focus, &QShortcut::activated, this, [this]() {
    if (search_bar_) search_bar_->FocusQuery();
  });

  auto *sc_preview = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_P), this);
  connect(sc_preview, &QShortcut::activated, this, &MainWindow::OnTogglePreview);

  auto *sc_settings = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Comma), this);
  connect(sc_settings, &QShortcut::activated, this, &MainWindow::OnOpenSettings);

  auto *sc_open_folder = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_O), this);
  connect(sc_open_folder, &QShortcut::activated, this, [this]() {
    if (preview_panel_) preview_panel_->OnOpenFolder();
  });

  auto *sc_open_enter = new QShortcut(QKeySequence(Qt::Key_Return), this);
  connect(sc_open_enter, &QShortcut::activated, this, [this]() {
    if (result_view_) {
      result_view_->OpenCurrentSelectedItem();
    }
  });

  auto *sc_open_enter_kp = new QShortcut(QKeySequence(Qt::Key_Enter), this);
  connect(sc_open_enter_kp, &QShortcut::activated, this, [this]() {
    if (result_view_) {
      result_view_->OpenCurrentSelectedItem();
    }
  });

  auto *sc_esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
  connect(sc_esc, &QShortcut::activated, this, [this]() {
    if (search_bar_ && !search_bar_->query().isEmpty()) {
      search_bar_->ClearQuery();
    } else {
      close();
    }
  });

  RetranslateUi();
}

void MainWindow::SetupTrayIcon() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    LOG_WARN(
        "[MainWindow] System tray is not currently reported as available by the desktop environment; attempting "
        "initialization anyway.");
  }

  tray_icon_ = new QSystemTrayIcon(this);
  tray_icon_->setIcon(IconProvider::GetAppIcon());
  tray_icon_->setToolTip(Tr("app_title"));

  tray_menu_ = new QMenu(this);
  tray_menu_->setStyleSheet(
      "QMenu {"
      "  background-color: #FFFFFF;"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "  padding: 4px 0px;"
      "}"
      "QMenu::item {"
      "  padding: 6px 24px 6px 16px;"
      "  font-size: 13px;"
      "  color: #1E293B;"
      "}"
      "QMenu::item:selected {"
      "  background-color: #E0F2FE;"
      "  color: #0284C7;"
      "}"
      "QMenu::separator {"
      "  height: 1px;"
      "  background-color: #E2E8F0;"
      "  margin: 4px 0px;"
      "}");

  action_show_ = tray_menu_->addAction(Tr("tray_show"), this, &MainWindow::OnTrayActionShow);
  action_settings_ = tray_menu_->addAction(Tr("tray_settings"), this, &MainWindow::OnOpenSettings);
  tray_menu_->addSeparator();
  action_quit_ = tray_menu_->addAction(Tr("tray_quit"), this, &MainWindow::OnTrayActionQuit);

  tray_icon_->setContextMenu(tray_menu_);

  connect(tray_icon_, &QSystemTrayIcon::activated, this, &MainWindow::OnTrayActivated);

  tray_icon_->show();
}

void MainWindow::OnTrayActivated(QSystemTrayIcon::ActivationReason reason) {
  if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
    ShowAndActivate();
  }
}

void MainWindow::ShowAndActivate() {
  showNormal();
  raise();
  activateWindow();
}

void MainWindow::OnTrayActionShow() {
  ShowAndActivate();
}

void MainWindow::OnTrayActionQuit() {
  force_quit_ = true;
  if (tray_icon_) {
    tray_icon_->hide();
  }
  close();
  qApp->quit();
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (force_quit_ || !tray_icon_ || !tray_icon_->isVisible()) {
    if (tray_icon_) {
      tray_icon_->hide();
    }
    event->accept();
    QMainWindow::closeEvent(event);
    qApp->quit();
  } else {
    event->ignore();
    hide();
    static bool has_shown_tray_tip = false;
    if (!has_shown_tray_tip) {
      has_shown_tray_tip = true;
      tray_icon_->showMessage(Tr("app_name"), Tr("tray_tip_msg"), QSystemTrayIcon::Information, 2000);
    }
  }
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
  if (status_preview_btn_) {
    status_preview_btn_->setText(Tr("btn_preview"));
    status_preview_btn_->setToolTip(Tr("tip_preview"));
  }
  if (tray_icon_) {
    tray_icon_->setToolTip(Tr("app_title"));
  }
  if (action_show_) {
    action_show_->setText(Tr("tray_show"));
  }
  if (action_settings_) {
    action_settings_->setText(Tr("tray_settings"));
  }
  if (action_quit_) {
    action_quit_->setText(Tr("tray_quit"));
  }
  UpdateStatusBar(total_matched_count_, last_search_cost_ms_);
}

void MainWindow::UpdateStatusBar(int total_matches, double cost_ms) {
  const size_t indexed_files = pipeline_ ? pipeline_->IndexedFileCount() : 0;

  status_stats_label_->setText(Tr("status_stats").arg(indexed_files).arg(total_matches));

  if (cost_ms > 0.0) {
    status_latency_label_->setText(Tr("status_latency").arg(QString::number(cost_ms, 'f', 2)));
  } else {
    status_latency_label_->setText(Tr("status_ready"));
  }
}

void MainWindow::OnSearchTriggered(const QString &query, int mode, const QString &path_scope) {
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
  auto keywords = SplitQueryKeywords(query_std);
  if (mode == kModeFulltext) {
    // 1. Full-text inverted search (asynchronous non-blocking search on UI thread)
    auto hits = pipeline_->SearchFulltext(query_std, kPageSize, 0, scope_std, /*sync_flush=*/false, &total_count);
    if (static_cast<int>(hits.size()) >= kPageSize && total_count > kPageSize) {
      has_more_results_ = true;
    }
    display_items = BuildFulltextDisplayItems(hits, keywords);
  } else {
    // 2. Instantaneous filename search
    auto name_hits = pipeline_->SearchFilename(query_std, kPageSize, 0, scope_std, &total_count);
    if (static_cast<int>(name_hits.size()) >= kPageSize && total_count > kPageSize) {
      has_more_results_ = true;
    }
    display_items = BuildFilenameDisplayItems(name_hits, keywords);
  }

  auto t1 = std::chrono::high_resolution_clock::now();
  double cost_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

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
  auto keywords = SplitQueryKeywords(query_std);

  if (current_search_mode_ == kModeFulltext) {
    auto hits =
        pipeline_->SearchFulltext(query_std, kPageSize, current_search_offset_, scope_std, /*sync_flush=*/false);
    if (static_cast<int>(hits.size()) < kPageSize ||
        current_search_offset_ + static_cast<int>(hits.size()) >= total_matched_count_) {
      has_more_results_ = false;
    }
    new_display_items = BuildFulltextDisplayItems(hits, keywords);
  } else {
    auto name_hits = pipeline_->SearchFilename(query_std, kPageSize, current_search_offset_, scope_std);
    if (static_cast<int>(name_hits.size()) < kPageSize ||
        current_search_offset_ + static_cast<int>(name_hits.size()) >= total_matched_count_) {
      has_more_results_ = false;
    }
    new_display_items = BuildFilenameDisplayItems(name_hits, keywords);
  }

  auto t1 = std::chrono::high_resolution_clock::now();
  double cost_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

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

void MainWindow::OnFileSelected(int64_t doc_id, const QString &file_name, const QString &file_path) {
  if (preview_panel_ && preview_panel_->isVisible()) {
    auto keywords = SplitQueryKeywords(current_query_.toStdString());
    preview_panel_->LoadDocument(doc_id, file_name, file_path, keywords);
  }
}

void MainWindow::OnTogglePreview() {
  if (preview_panel_) {
    bool target_visible = !preview_panel_->isVisible();
    preview_panel_->setVisible(target_visible);
    if (status_preview_btn_) {
      status_preview_btn_->setChecked(target_visible);
    }
  }
}

void MainWindow::OnFileOpenRequested(int64_t doc_id, const QString &file_name, const QString &file_path) {
  OnFileSelected(doc_id, file_name, file_path);

  // Modern desktop search standard: double click directly opens with OS default program
  bool opened = QDesktopServices::openUrl(QUrl::fromLocalFile(file_path));
  if (!opened) {
    auto keywords = SplitQueryKeywords(current_query_.toStdString());
    auto coordinator = pipeline_ ? pipeline_->search_coordinator() : nullptr;
    ContentPreviewDialog dlg(doc_id, file_name, file_path, keywords, coordinator, this);
    dlg.exec();
  }
}

void MainWindow::OnOpenSettings() {
  if (!pipeline_ || !pipeline_->config()) {
    return;
  }
  SettingsDialog dlg(pipeline_->config(), this);
  connect(&dlg, &SettingsDialog::SettingsApplied, this, &MainWindow::OnSettingsApplied);
  connect(&dlg, &SettingsDialog::RebuildIndexRequested, this, &MainWindow::OnRebuildIndexRequested);
  dlg.exec();
}

void MainWindow::OnSettingsApplied() {
  RetranslateUi();

  if (pipeline_) {
    pipeline_->ApplyRuntimeConfig();
  }

  // Clear any temporary statusBar message so progress bar is visible immediately
  statusBar()->clearMessage();

  // Trigger background indexing for newly added or modified directories
  StartBackgroundInitialIndex(true);

  // Re-trigger current search only if idle and there is an active search query
  if (!is_task_active_ && (!pipeline_ || !pipeline_->IsIndexing()) && search_bar_ &&
      (!search_bar_->query().trimmed().isEmpty() || !search_bar_->path_scope().trimmed().isEmpty())) {
    OnSearchTriggered(search_bar_->query(), search_bar_->mode(), search_bar_->path_scope());
  }
}

void MainWindow::OnRebuildIndexRequested() {
  if (!pipeline_) {
    return;
  }
  statusBar()->clearMessage();
  pipeline_->StartBackgroundTask(true, false, MakeBackgroundCallbacks());
}

void MainWindow::StartBackgroundInitialIndex(bool force_incremental) {
  if (!pipeline_) {
    return;
  }
  pipeline_->StartBackgroundTask(false, force_incremental, MakeBackgroundCallbacks());
}

service::BackgroundIndexCallbacks MainWindow::MakeBackgroundCallbacks() {
  service::BackgroundIndexCallbacks callbacks;
  const std::shared_ptr<CallbackTarget> target = callback_target_;

  callbacks.on_started = [target](service::IndexTaskKind kind, uint64_t generation) {
    if (!target) return;
    std::lock_guard<std::mutex> lock(target->mutex);
    if (target->window) {
      emit target->window->IndexTaskStarted(static_cast<int>(kind), static_cast<quint64>(generation));
    }
  };
  callbacks.on_progress = [target](service::IndexTaskKind kind, uint64_t generation, size_t indexed, size_t total,
                                   const std::string &path) {
    if (!target) return;
    std::lock_guard<std::mutex> lock(target->mutex);
    if (target->window) {
      emit target->window->IndexProgressUpdated(static_cast<int>(kind), static_cast<int>(indexed),
                                                static_cast<int>(total), QString::fromStdString(path),
                                                static_cast<quint64>(generation));
    }
  };
  callbacks.on_finished = [target](const service::IndexTaskResult &result) {
    if (!target) return;
    std::lock_guard<std::mutex> lock(target->mutex);
    if (target->window) {
      emit target->window->IndexFinished(static_cast<int>(result.kind), static_cast<int>(result.total_indexed),
                                         result.cancelled, result.elapsed_sec, result.has_pending_followup,
                                         static_cast<quint64>(result.generation));
    }
  };
  return callbacks;
}

void MainWindow::OnIndexTaskStarted(int task_type, quint64 generation) {
  active_generation_ = generation;
  is_task_active_ = true;

  statusBar()->clearMessage();
  status_progress_bar_->setValue(0);
  status_progress_bar_->setVisible(true);
  status_cancel_btn_->setVisible(true);
  status_cancel_btn_->setEnabled(true);
  status_file_label_->setText(Tr("status_init_manifest"));
  status_file_label_->setVisible(true);

  const QString badge = IndexTaskBadge(task_type);
  if (task_type == static_cast<int>(service::IndexTaskKind::kRebuild)) {
    status_stats_label_->setText(badge + Tr("status_rebuild_scanning"));
    status_latency_label_->setText(Tr("status_state_rebuilding"));
  } else {
    status_stats_label_->setText(badge + Tr("status_scanning_dirs"));
    status_latency_label_->setText(task_type == static_cast<int>(service::IndexTaskKind::kInitial)
                                       ? Tr("status_state_initializing")
                                       : Tr("status_state_syncing"));
  }
}

void MainWindow::OnIndexProgressUpdated(int task_type, int indexed, int total, const QString &current_file,
                                        quint64 generation) {
  if (!is_task_active_ || generation != active_generation_) {
    // Drop late progress events from a task that already finished or was superseded.
    return;
  }
  const QString badge = IndexTaskBadge(task_type);

  if (total > 0) {
    const int pct = static_cast<int>(std::clamp<int64_t>((static_cast<int64_t>(indexed) * 100) / total, 0, 100));
    status_progress_bar_->setValue(pct);
    status_progress_bar_->setVisible(true);
    status_stats_label_->setText(badge + Tr("status_building_progress").arg(indexed).arg(total).arg(pct));
  } else {
    status_stats_label_->setText(badge + Tr("status_scanning_dirs"));
  }

  if (!current_file.isEmpty()) {
    status_file_label_->setVisible(true);
    QFontMetrics fm(status_file_label_->font());
    QString elided = fm.elidedText(Tr("status_parsing_file").arg(current_file), Qt::ElideMiddle, 460);
    status_file_label_->setText(elided);
    status_file_label_->setToolTip(current_file);
  }
}

void MainWindow::OnIndexFinished(int task_type, int total_indexed, bool cancelled, double elapsed_sec,
                                 bool has_followup, quint64 generation) {
  if (generation != active_generation_) {
    LOG_DEBUG("[MainWindow] Ignoring stale IndexFinished (generation {} != {})", generation, active_generation_);
    return;
  }

  is_task_active_ = false;

  if (has_followup) {
    return;
  }

  status_progress_bar_->setVisible(false);
  status_file_label_->setVisible(false);
  status_cancel_btn_->setVisible(false);

  if (cancelled) {
    status_stats_label_->setText(Tr("status_task_cancelled_msg"));
    status_latency_label_->setText(Tr("status_ready"));
  } else {
    QString finish_desc;
    if (task_type == static_cast<int>(service::IndexTaskKind::kRebuild)) {
      finish_desc = Tr("status_finish_rebuild").arg(total_indexed).arg(QString::number(elapsed_sec, 'f', 1));
    } else if (task_type == static_cast<int>(service::IndexTaskKind::kInitial)) {
      finish_desc = Tr("status_finish_initial").arg(total_indexed).arg(QString::number(elapsed_sec, 'f', 1));
    } else {
      finish_desc = Tr("status_finish_incremental").arg(QString::number(elapsed_sec, 'f', 1));
    }
    status_stats_label_->setText(finish_desc);
    status_latency_label_->setText(Tr("status_ready"));
  }

  // Smoothly restore normal idle statistics after 3.5 seconds
  QTimer::singleShot(3500, this, [this]() {
    if (!is_task_active_ && (!pipeline_ || !pipeline_->IsIndexing())) {
      UpdateStatusBar(total_matched_count_, last_search_cost_ms_);
    }
  });

  // If search bar has active query, re-run search to surface newly indexed items immediately
  if (search_bar_ && (!search_bar_->query().trimmed().isEmpty() || !search_bar_->path_scope().trimmed().isEmpty())) {
    OnSearchTriggered(search_bar_->query(), search_bar_->mode(), search_bar_->path_scope());
  }
}

}  // namespace gui
}  // namespace soeasy
