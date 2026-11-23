// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_INDEX_PROGRESS_DIALOG_H_
#define SOEASY_SRC_GUI_INDEX_PROGRESS_DIALOG_H_

#include <QCloseEvent>
#include <QDialog>
#include <QHideEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QTableWidget>
#include <QTimer>
#include <memory>

#include "service/index_pipeline.h"

namespace soeasy {
namespace gui {

/**
 * @brief Non-modal dialog displaying overall indexing progress and per-worker in-flight files.
 */
class IndexProgressDialog : public QDialog {
  Q_OBJECT

 public:
  explicit IndexProgressDialog(std::shared_ptr<service::IndexPipeline> pipeline, QWidget *parent = nullptr);
  ~IndexProgressDialog() override = default;

  void StartTracking(int fallback_task_type = 0);
  void HandleTaskFinished();
  void RetranslateUi();

 signals:
  void CancelRequested();

 public slots:
  void Refresh();

 protected:
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;
  void closeEvent(QCloseEvent *event) override;

 private:
  void SetupUi();
  void SyncCancelButtonState();

  std::shared_ptr<service::IndexPipeline> pipeline_;
  int fallback_task_type_ = 0;
  bool saw_active_scan_ = false;

  QLabel *summary_label_ = nullptr;
  QProgressBar *progress_bar_ = nullptr;
  QTableWidget *worker_table_ = nullptr;
  QPushButton *cancel_btn_ = nullptr;
  QPushButton *close_btn_ = nullptr;
  QTimer *refresh_timer_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_INDEX_PROGRESS_DIALOG_H_
