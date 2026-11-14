#ifndef SOEASY_SRC_GUI_MAIN_WINDOW_H_
#define SOEASY_SRC_GUI_MAIN_WINDOW_H_

#include <QAction>
#include <QCloseEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gui/content_preview_dialog.h"
#include "gui/preview_panel.h"
#include "gui/result_view.h"
#include "gui/search_bar.h"
#include "gui/settings_dialog.h"
#include "gui/title_bar.h"
#include "service/index_pipeline.h"

namespace soeasy {
namespace gui {

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(std::shared_ptr<service::IndexPipeline> pipeline, QWidget *parent = nullptr);
  ~MainWindow() override;

  /**
   * @brief Trigger non-blocking asynchronous initial scan and indexing in the background.
   */
  void StartBackgroundInitialIndex(bool force_incremental = false);

  /**
   * @brief Bring window to foreground, restoring from minimized or hidden/tray state.
   */
  void ShowAndActivate();

 signals:
  void IndexTaskStarted(int task_type, quint64 generation);
  void IndexProgressUpdated(int task_type, int indexed, int total, const QString &current_file, quint64 generation);
  void IndexFinished(int task_type, int total_indexed, bool cancelled, double elapsed_sec, bool has_followup,
                     quint64 generation);

 protected:
  void changeEvent(QEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void closeEvent(QCloseEvent *event) override;

 private slots:
  void OnSearchTriggered(const QString &query, int mode, const QString &path_scope);
  void OnFileSelected(int64_t doc_id, const QString &file_name, const QString &file_path);
  void OnFileOpenRequested(int64_t doc_id, const QString &file_name, const QString &file_path);
  void OnLoadMoreRequested();
  void OnTogglePreview();
  void OnOpenSettings();
  void OnSettingsApplied();
  void OnRebuildIndexRequested();

  void OnIndexTaskStarted(int task_type, quint64 generation);
  void OnIndexProgressUpdated(int task_type, int indexed, int total, const QString &current_file, quint64 generation);
  void OnIndexFinished(int task_type, int total_indexed, bool cancelled, double elapsed_sec, bool has_followup,
                       quint64 generation);

  void OnTrayActivated(QSystemTrayIcon::ActivationReason reason);
  void OnTrayActionShow();
  void OnTrayActionQuit();

 private:
  struct CallbackTarget {
    std::mutex mutex;
    MainWindow *window = nullptr;
  };

  void SetupUi();
  void SetupTrayIcon();
  void RetranslateUi();
  void UpdateStatusBar(int total_matches, double cost_ms);
  [[nodiscard]] service::BackgroundIndexCallbacks MakeBackgroundCallbacks();

  std::shared_ptr<service::IndexPipeline> pipeline_;

  TitleBar *title_bar_ = nullptr;
  SearchBar *search_bar_ = nullptr;
  QSplitter *splitter_ = nullptr;
  ResultView *result_view_ = nullptr;
  PreviewPanel *preview_panel_ = nullptr;

  QLabel *status_stats_label_ = nullptr;
  QProgressBar *status_progress_bar_ = nullptr;
  QPushButton *status_cancel_btn_ = nullptr;
  QLabel *status_file_label_ = nullptr;
  QPushButton *status_preview_btn_ = nullptr;
  QLabel *status_latency_label_ = nullptr;

  QString current_query_;
  int current_search_mode_ = 0;
  QString current_search_path_scope_;
  int current_search_offset_ = 0;
  int total_matched_count_ = 0;
  double last_search_cost_ms_ = 0.0;
  bool has_more_results_ = false;
  bool is_loading_more_ = false;
  static constexpr int kPageSize = 100;

  std::shared_ptr<CallbackTarget> callback_target_;
  bool is_task_active_{false};
  quint64 active_generation_{0};
  bool splitter_initialized_{false};

  QSystemTrayIcon *tray_icon_ = nullptr;
  QMenu *tray_menu_ = nullptr;
  QAction *action_show_ = nullptr;
  QAction *action_settings_ = nullptr;
  QAction *action_quit_ = nullptr;
  bool force_quit_{false};
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_MAIN_WINDOW_H_
