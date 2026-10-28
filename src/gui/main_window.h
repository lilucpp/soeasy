#ifndef SOEASY_SRC_GUI_MAIN_WINDOW_H_
#define SOEASY_SRC_GUI_MAIN_WINDOW_H_

#include <QLabel>
#include <QMainWindow>
#include <QSplitter>
#include <QStatusBar>

#include <memory>
#include <string>
#include <vector>

#include "gui/content_preview_dialog.h"
#include "gui/preview_panel.h"
#include "gui/result_view.h"
#include "gui/search_bar.h"
#include "gui/settings_dialog.h"
#include "gui/title_bar.h"
#include "service/index_pipeline.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <QPushButton>
#include <QProgressBar>

namespace soeasy {
namespace gui {

enum IndexTaskType {
  kTaskInitial = 0,
  kTaskRebuild = 1,
  kTaskIncremental = 2,
};

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(std::shared_ptr<service::IndexPipeline> pipeline,
                      QWidget* parent = nullptr);
  ~MainWindow() override;

  /**
   * @brief Trigger non-blocking asynchronous initial scan and indexing in the background.
   */
  void StartBackgroundInitialIndex(bool force_incremental = false);

 signals:
  void IndexProgressUpdated(int task_type, int indexed, int total, const QString& current_file);
  void IndexFinished(int task_type, int total_indexed, bool cancelled, double elapsed_sec);

 protected:
  void changeEvent(QEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private slots:
  void OnSearchTriggered(const QString& query, int mode, const QString& path_scope);
  void OnFileSelected(int64_t doc_id, const QString& file_name,
                      const QString& file_path);
  void OnFileOpenRequested(int64_t doc_id, const QString& file_name,
                           const QString& file_path);
  void OnLoadMoreRequested();
  void OnTogglePreview();
  void OnOpenSettings();
  void OnSettingsApplied();
  void OnRebuildIndexRequested();

  void OnIndexProgressUpdated(int task_type, int indexed, int total, const QString& current_file);
  void OnIndexFinished(int task_type, int total_indexed, bool cancelled, double elapsed_sec);

 private:
  void SetupUi();
  void UpdateStatusBar(int total_matches, double cost_ms);

  std::shared_ptr<service::IndexPipeline> pipeline_;

  TitleBar* title_bar_ = nullptr;
  SearchBar* search_bar_ = nullptr;
  QSplitter* splitter_ = nullptr;
  ResultView* result_view_ = nullptr;
  PreviewPanel* preview_panel_ = nullptr;

  QLabel* status_stats_label_ = nullptr;
  QProgressBar* status_progress_bar_ = nullptr;
  QPushButton* status_cancel_btn_ = nullptr;
  QLabel* status_file_label_ = nullptr;
  QLabel* status_latency_label_ = nullptr;

  QString current_query_;
  int current_search_mode_ = 0;
  QString current_search_path_scope_;
  int current_search_offset_ = 0;
  int total_matched_count_ = 0;
  double last_search_cost_ms_ = 0.0;
  bool has_more_results_ = false;
  bool is_loading_more_ = false;
  static constexpr int kPageSize = 100;

  std::thread background_index_thread_;
  std::atomic<bool> is_indexing_{false};
  std::atomic<bool> pending_reindex_{false};
  bool splitter_initialized_{false};
  int current_task_type_{kTaskInitial};
  std::chrono::steady_clock::time_point index_start_time_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_MAIN_WINDOW_H_
