// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_DOCUMENT_VIEWER_WIDGET_H_
#define SOEASY_SRC_GUI_DOCUMENT_VIEWER_WIDGET_H_

#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTextBrowser>
#include <QWidget>
#include <memory>
#include <string>
#include <vector>

#include "service/isearch_coordinator.h"

namespace soeasy {
namespace gui {

/**
 * @brief Reusable document preview widget with in-document keyword highlighting and navigation.
 *
 * Used by PreviewPanel (main window master-detail) and ContentPreviewDialog (modal preview).
 */
class DocumentViewerWidget : public QWidget {
  Q_OBJECT

 public:
  explicit DocumentViewerWidget(QWidget *parent = nullptr);
  ~DocumentViewerWidget() override = default;

  void LoadContent(const QString &text, const std::vector<std::string> &keywords);
  void SetHtml(const QString &html);
  void Clear();
  void RetranslateUi();

  [[nodiscard]] QTextBrowser *text_viewer() const { return text_viewer_; }
  [[nodiscard]] int match_count() const { return static_cast<int>(match_ranges_.size()); }

  /**
   * @brief Reads document content via CQRS query coordinator, with fallback to direct disk read (up to 1MB).
   */
  static bool ReadDocumentContent(int64_t doc_id, const QString &file_path,
                                  const std::shared_ptr<service::ISearchCoordinator> &coordinator,
                                  std::string *out_content);

 public slots:
  void FindNext();
  void FindPrev();

 private:
  void SetupUi();
  void HighlightOccurrences();
  void UpdateHighlightSelections();
  void UpdateMatchCountLabel();
  void JumpToMatch(int index);

  struct MatchRange {
    int start = 0;
    int end = 0;
  };

  QLabel *label_jump_ = nullptr;
  QLabel *keyword_tag_label_ = nullptr;
  QPushButton *btn_prev_ = nullptr;
  QPushButton *btn_next_ = nullptr;
  QLabel *match_count_label_ = nullptr;
  QTextBrowser *text_viewer_ = nullptr;

  std::vector<std::string> current_keywords_;
  std::vector<MatchRange> match_ranges_;
  int current_match_idx_ = -1;
  bool highlights_truncated_ = false;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_DOCUMENT_VIEWER_WIDGET_H_
