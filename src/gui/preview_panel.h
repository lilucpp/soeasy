#ifndef SOEASY_SRC_GUI_PREVIEW_PANEL_H_
#define SOEASY_SRC_GUI_PREVIEW_PANEL_H_

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTextBrowser>
#include <QWidget>
#include <memory>
#include <string>
#include <vector>

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

 public slots:
  void OnOpenFolder();
  void OnOpenWithDefaultApp();

 private slots:
  void OnFindNext();
  void OnFindPrev();

 private:
  void SetupUi();
  void RenderEmptyStateGuide();
  void HighlightOccurrences();
  void UpdateHighlightSelections();
  void JumpToMatch(int index);

  struct MatchRange {
    int start = 0;
    int end = 0;
  };

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

  QLabel *label_jump_ = nullptr;
  QLabel *keyword_tag_label_ = nullptr;
  QPushButton *btn_prev_ = nullptr;
  QPushButton *btn_next_ = nullptr;
  QLabel *match_count_label_ = nullptr;

  QTextBrowser *text_viewer_ = nullptr;

  std::vector<MatchRange> match_ranges_;
  int current_match_idx_ = -1;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_PREVIEW_PANEL_H_
