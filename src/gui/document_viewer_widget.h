// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_DOCUMENT_VIEWER_WIDGET_H_
#define SOEASY_SRC_GUI_DOCUMENT_VIEWER_WIDGET_H_

#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QString>
#include <QTextBrowser>
#include <QWidget>
#include <memory>
#include <string>
#include <vector>

#include "common/parse_status.h"
#include "gui/gui_utils.h"
#include "service/isearch_coordinator.h"

namespace soeasy {
namespace gui {

class KnuthPlassViewer;
class TypographyPopover;

struct DocumentPreviewResult {
  bool has_content = false;
  bool truncated = false;
  std::string content;
  common::ParseStatus parse_status = common::ParseStatus::kUnknown;
  std::string issue_reason;
};

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

  void LoadContent(const QString &text, const std::vector<std::string> &keywords,
                   const QString &file_path = QString());
  void SetHtml(const QString &html);
  void Clear();
  void RetranslateUi();

  [[nodiscard]] QTextBrowser *text_viewer() const { return text_viewer_; }
  [[nodiscard]] KnuthPlassViewer *kp_viewer() const { return kp_viewer_; }
  [[nodiscard]] bool is_kp_active() const { return kp_active_for_doc_; }
  [[nodiscard]] bool is_code_document() const { return is_code_document_; }
  [[nodiscard]] int match_count() const;

  static bool IsCodeOrStructuredFile(const QString &file_path);

  /**
   * @brief Load document content and parse-status metadata via CQRS query coordinator,
   * falling back to direct disk read only for plain-text formats.
   */
  static DocumentPreviewResult LoadPreviewDocument(
      int64_t doc_id, const QString &file_path,
      const std::shared_ptr<service::ISearchCoordinator> &coordinator);

  /**
   * @brief Format loaded preview content on the UI thread (appending localized truncation notice if needed).
   */
  static QString FormatPreviewText(const DocumentPreviewResult &preview);

  /**
   * @brief Render user-friendly notice HTML when a document has no previewable text
   * (encrypted, failed, timed out, crashed, or empty/blank).
   */
  static QString BuildStatusNoticeHtml(const DocumentPreviewResult &preview);

  /**
   * @brief Reads document content via CQRS query coordinator, with plain-text fallback to disk (up to 1MB).
   */
  static bool ReadDocumentContent(int64_t doc_id, const QString &file_path,
                                  const std::shared_ptr<service::ISearchCoordinator> &coordinator,
                                  std::string *out_content);

 public slots:
  void FindNext();
  void FindPrev();

 private:
  void SetupUi();
  void ApplyActiveViewerMode(bool use_kp);
  void ApplyBrowserStyle(bool code_mode);
  void ApplyBrowserBlockSpacing(double line_height_pct);
  void UpdateKeywordBadge(const std::vector<std::string> &keywords);
  void HighlightOccurrences();
  void UpdateHighlightSelections();
  void UpdateMatchCountLabel();
  void JumpToMatch(int index);

  struct MatchRange {
    int start = 0;
    int end = 0;
  };

  QLabel *label_jump_ = nullptr;
  ElidedLabel *keyword_tag_label_ = nullptr;
  QPushButton *btn_prev_ = nullptr;
  QPushButton *btn_next_ = nullptr;
  QLabel *match_count_label_ = nullptr;
  QPushButton *btn_kp_toggle_ = nullptr;
  QPushButton *btn_typography_ = nullptr;
  QStackedWidget *viewer_stack_ = nullptr;
  QTextBrowser *text_viewer_ = nullptr;
  KnuthPlassViewer *kp_viewer_ = nullptr;
  TypographyPopover *typography_popover_ = nullptr;

  std::vector<std::string> current_keywords_;
  std::vector<MatchRange> match_ranges_;
  int current_match_idx_ = -1;
  bool highlights_truncated_ = false;
  QString current_plain_text_;
  QString current_file_path_;
  bool has_plain_text_ = false;
  bool html_mode_ = false;
  bool is_code_document_ = false;
  bool kp_active_for_doc_ = false;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_DOCUMENT_VIEWER_WIDGET_H_
