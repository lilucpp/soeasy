#ifndef SOEASY_SRC_GUI_CONTENT_PREVIEW_DIALOG_H_
#define SOEASY_SRC_GUI_CONTENT_PREVIEW_DIALOG_H_

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTextBrowser>

#include <memory>
#include <string>
#include <vector>

#include "storage/storage_engine.h"

namespace soeasy {
namespace gui {

class ContentPreviewDialog : public QDialog {
  Q_OBJECT

 public:
  ContentPreviewDialog(int64_t doc_id, const QString& file_name,
                       const QString& file_path,
                       const std::vector<std::string>& keywords,
                       std::shared_ptr<storage::StorageEngine> storage,
                       QWidget* parent = nullptr);
  ~ContentPreviewDialog() override = default;

 private slots:
  void OnFindNext();
  void OnFindPrev();
  void OnOpenFolder();
  void OnOpenWithDefaultApp();

 private:
  void SetupUi();
  void LoadContentAndHighlight();
  void HighlightOccurrences();
  void JumpToMatch(int index);

  int64_t doc_id_ = 0;
  QString file_name_;
  QString file_path_;
  std::vector<std::string> keywords_;
  std::shared_ptr<storage::StorageEngine> storage_;

  QTextBrowser* text_viewer_ = nullptr;
  QLabel* path_label_ = nullptr;
  QLabel* match_count_label_ = nullptr;
  QLineEdit* in_doc_search_edit_ = nullptr;

  std::vector<int> match_positions_;
  int current_match_idx_ = -1;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_CONTENT_PREVIEW_DIALOG_H_
