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

namespace soeasy {
namespace gui {

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(std::shared_ptr<service::IndexPipeline> pipeline,
                      QWidget* parent = nullptr);
  ~MainWindow() override = default;

 protected:
  void changeEvent(QEvent* event) override;

 private slots:
  void OnSearchTriggered(const QString& query, int mode, const QString& path_scope);
  void OnFileSelected(int64_t doc_id, const QString& file_name,
                      const QString& file_path);
  void OnFileOpenRequested(int64_t doc_id, const QString& file_name,
                           const QString& file_path);
  void OnTogglePreview();
  void OnOpenSettings();
  void OnSettingsApplied();
  void OnRebuildIndexRequested();

 private:
  void SetupUi();
  void UpdateStatusBar(int match_count, double cost_ms);

  std::shared_ptr<service::IndexPipeline> pipeline_;

  TitleBar* title_bar_ = nullptr;
  SearchBar* search_bar_ = nullptr;
  QSplitter* splitter_ = nullptr;
  ResultView* result_view_ = nullptr;
  PreviewPanel* preview_panel_ = nullptr;

  QLabel* status_stats_label_ = nullptr;
  QLabel* status_latency_label_ = nullptr;

  QString current_query_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_MAIN_WINDOW_H_
