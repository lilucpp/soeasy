// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_PREVIEW_PANEL_H_
#define SOEASY_SRC_GUI_PREVIEW_PANEL_H_

#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QWidget>
#include <memory>
#include <string>
#include <vector>

#include "gui/document_viewer_widget.h"
#include "storage/storage_engine.h"

namespace soeasy {
namespace gui {

class PreviewPanel : public QWidget {
  Q_OBJECT

 public:
  explicit PreviewPanel(QWidget *parent = nullptr);
  ~PreviewPanel() override = default;

  void SetStorageEngine(std::shared_ptr<storage::StorageEngine> storage);

  void LoadDocument(int64_t doc_id, const QString &file_name, const QString &file_path,
                    const std::vector<std::string> &keywords);
  void ClearPreview();
  void RetranslateUi();

  [[nodiscard]] DocumentViewerWidget *viewer() const { return viewer_; }
  void WaitForLoadFinished(int timeout_ms = 1000);

 signals:
  void CloseRequested();

 public slots:
  void OnOpenFolder();
  void OnOpenWithDefaultApp();

 private:
  void SetupUi();
  void RenderEmptyStateGuide();

  uint64_t load_request_id_ = 0;
  int64_t current_doc_id_ = 0;
  QString current_file_name_;
  QString current_file_path_;
  std::vector<std::string> current_keywords_;
  std::shared_ptr<storage::StorageEngine> storage_;

  // UI elements
  QLabel *icon_label_ = nullptr;
  QLabel *title_label_ = nullptr;
  QLabel *path_label_ = nullptr;
  QPushButton *btn_open_file_ = nullptr;
  QPushButton *btn_open_folder_ = nullptr;
  QPushButton *btn_close_ = nullptr;

  DocumentViewerWidget *viewer_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_PREVIEW_PANEL_H_
