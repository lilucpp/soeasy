// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_RESULT_VIEW_H_
#define SOEASY_SRC_GUI_RESULT_VIEW_H_

#include <QHeaderView>
#include <QMouseEvent>
#include <QTableView>
#include <QTimer>
#include <memory>
#include <unordered_map>
#include <vector>

#include "gui/hover_preview_popup.h"
#include "gui/search_table_model.h"
#include "storage/storage_engine.h"

namespace soeasy {
namespace gui {

/**
 * @brief High-performance virtual viewport search results view backed by SearchTableModel.
 *
 * Inherits from QTableView instead of QTableWidget to achieve lazy cell rendering,
 * smooth 60fps scrolling, and memory-efficient result inspection.
 */
class ResultView : public QTableView {
  Q_OBJECT

 public:
  explicit ResultView(QWidget *parent = nullptr);
  ~ResultView() override = default;

  void SetStorageEngine(std::shared_ptr<storage::StorageEngine> storage);
  void SetCurrentQuery(const QString &query);
  void SetResults(const std::vector<DisplayResultItem> &items);
  void AppendResults(const std::vector<DisplayResultItem> &items);
  void ClearResults();
  void RetranslateUi();

  void EnsureSnippetForRows(int start_row, int end_row);
  void OpenCurrentSelectedItem();

  [[nodiscard]] int rowCount() const { return model_ ? model_->rowCount() : 0; }
  [[nodiscard]] int columnCount() const { return model_ ? model_->columnCount() : 0; }
  [[nodiscard]] SearchTableModel *search_model() const { return model_; }

  // Backward compatibility proxy for test inspection
  struct CompatItemProxy {
    QString text_val;
    QString text() const { return text_val; }
  };
  [[nodiscard]] std::unique_ptr<CompatItemProxy> item(int row, int col) const;
  [[nodiscard]] std::unique_ptr<CompatItemProxy> horizontalHeaderItem(int section) const;

 signals:
  void FileSelected(int64_t doc_id, const QString &file_name, const QString &file_path);
  void FileOpenRequested(int64_t doc_id, const QString &file_name, const QString &file_path);
  void LoadMoreRequested();

 protected:
  void keyPressEvent(QKeyEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;

 private slots:
  void OnDoubleClicked(const QModelIndex &index);
  void OnCurrentChanged(const QModelIndex &current, const QModelIndex &previous);
  void OnHoverTimerFired();
  void OnScrollIdle();

 private:
  void SetupTable();

  SearchTableModel *model_ = nullptr;
  HoverPreviewPopup *hover_popup_ = nullptr;
  QTimer *hover_timer_ = nullptr;
  QTimer *scroll_timer_ = nullptr;

  std::shared_ptr<storage::StorageEngine> storage_;
  QString current_query_;
  std::unordered_map<int64_t, std::string> snippet_lru_cache_;

  int hover_row_ = -1;
  QPoint hover_pos_;

  int last_sorted_col_ = -1;
  Qt::SortOrder last_sort_order_ = Qt::AscendingOrder;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_RESULT_VIEW_H_
