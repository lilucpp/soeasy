#ifndef SOEASY_SRC_GUI_RESULT_VIEW_H_
#define SOEASY_SRC_GUI_RESULT_VIEW_H_

#include <QHeaderView>
#include <QMouseEvent>
#include <QTableWidget>
#include <QTimer>

#include <memory>
#include <vector>

#include "gui/hover_preview_popup.h"
#include "service/name_index.h"
#include "storage/storage_engine.h"

namespace soeasy {
namespace gui {

struct DisplayResultItem {
  int64_t doc_id = 0;
  QString file_name;
  QString file_path;
  QString ext;
  uint64_t file_size = 0;
  int64_t mtime = 0;
  double score = 0.0;
  QString snippet_html;
  bool has_body_snippet = false;
};

class ResultView : public QTableWidget {
  Q_OBJECT

 public:
  explicit ResultView(QWidget* parent = nullptr);
  ~ResultView() override = default;

  void SetStorageEngine(std::shared_ptr<storage::StorageEngine> storage);
  void SetCurrentQuery(const QString& query);
  void SetResults(const std::vector<DisplayResultItem>& items);
  void AppendResults(const std::vector<DisplayResultItem>& items);
  void ClearResults();
  void RetranslateUi();

  void EnsureSnippetForRows(int start_row, int end_row);

 signals:
  void FileSelected(int64_t doc_id, const QString& file_name,
                    const QString& file_path);
  void FileOpenRequested(int64_t doc_id, const QString& file_name,
                         const QString& file_path);
  void LoadMoreRequested();

 protected:
  void mouseMoveEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;

 private slots:
  void OnItemDoubleClicked(int row, int column);
  void OnHoverTimerFired();
  void OnScrollIdle();

 private:
  void SetupTable();
  void PopulateRow(int row, const DisplayResultItem& item);

  std::vector<DisplayResultItem> current_items_;
  HoverPreviewPopup* hover_popup_ = nullptr;
  QTimer* hover_timer_ = nullptr;
  QTimer* scroll_timer_ = nullptr;

  std::shared_ptr<storage::StorageEngine> storage_;
  QString current_query_;
  std::unordered_map<int64_t, std::string> snippet_lru_cache_;

  int hover_row_ = -1;
  QPoint hover_pos_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_RESULT_VIEW_H_
