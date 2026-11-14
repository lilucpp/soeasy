// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_CONTENT_PREVIEW_DIALOG_H_
#define SOEASY_SRC_GUI_CONTENT_PREVIEW_DIALOG_H_

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gui/document_viewer_widget.h"
#include "service/isearch_coordinator.h"

namespace soeasy {
namespace gui {

class ContentPreviewDialog : public QDialog {
  Q_OBJECT

 public:
  ContentPreviewDialog(int64_t doc_id, QString file_name, QString file_path, std::vector<std::string> keywords,
                       std::shared_ptr<service::ISearchCoordinator> coordinator, QWidget *parent = nullptr);
  ~ContentPreviewDialog() override = default;

 private slots:
  void OnOpenFolder();
  void OnOpenWithDefaultApp();

 private:
  void SetupUi();
  void LoadContentAndHighlight();

  int64_t doc_id_ = 0;
  QString file_name_;
  QString file_path_;
  std::vector<std::string> keywords_;
  std::shared_ptr<service::ISearchCoordinator> search_coordinator_;

  DocumentViewerWidget *viewer_ = nullptr;
  QLabel *path_label_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_CONTENT_PREVIEW_DIALOG_H_
