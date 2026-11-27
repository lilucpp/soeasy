// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_KNUTH_PLASS_VIEWER_H_
#define SOEASY_SRC_GUI_KNUTH_PLASS_VIEWER_H_

#include <QAbstractScrollArea>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <vector>

#include "gui/typesetting/preview_typography.h"

namespace soeasy {
namespace gui {

namespace typesetting {
class ITextMetrics;
struct TypesetStyle;
}  // namespace typesetting

/**
 * @brief Scrollable Qt5 reader widget rendering prose paragraphs using Knuth-Plass
 * dynamic programming, lazy viewport breaking, contiguous keyword highlights,
 * match navigation, and mouse text selection/copy.
 */
class KnuthPlassViewer : public QAbstractScrollArea {
  Q_OBJECT

 public:
  explicit KnuthPlassViewer(QWidget *parent = nullptr);
  ~KnuthPlassViewer() override = default;

  void SetContent(const QString &text, const std::vector<std::string> &keywords);
  void Clear();
  void SetTypographyConfig(const typesetting::PreviewTypographyConfig &cfg);

  int MatchCount() const { return static_cast<int>(matches_.size()); }
  int CurrentMatchIndex() const { return current_match_idx_; }
  bool HighlightsTruncated() const { return highlights_truncated_; }
  void JumpToMatch(int index);

  QString SelectedText() const;
  QString PlainJoinedText() const;
  void SelectAllText();
  void CopySelection() const;

  // Exposed for automated layout/lazy-viewport tests
  size_t ParagraphCount() const { return paragraphs_.size(); }
  size_t LaidOutParagraphCount() const;

 protected:
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;

 private:
  struct RenderedBox {
    double x = 0.0;
    double width = 0.0;
    double draw_dx = 0.0;
    QString text;
    int src_start = -1;
    int src_end = -1;
  };

  struct RenderedLine {
    double y_baseline = 0.0;  // relative to paragraph top
    double line_end_x = 0.0;  // rightmost edge of last box on line
    int src_start = -1;
    int src_end = -1;
    std::vector<RenderedBox> boxes;
    QString trailing_hyphen;
    double hyphen_x = 0.0;
  };

  struct ParagraphEntry {
    QString raw_text;
    QString joined_text;
    bool laid_out = false;
    double y_top = 0.0;
    double height = 0.0;
    double laid_width = -1.0;
    std::vector<RenderedLine> lines;
  };

  struct MatchLocation {
    size_t para_idx = 0;
    int start = 0;  // UTF-16 offset in joined_text
    int end = 0;
  };

  struct TextPos {
    int para_idx = -1;
    int char_idx = 0;

    bool IsValid() const { return para_idx >= 0; }
    bool operator<(const TextPos &other) const {
      if (para_idx != other.para_idx) return para_idx < other.para_idx;
      return char_idx < other.char_idx;
    }
    bool operator==(const TextPos &other) const {
      return para_idx == other.para_idx && char_idx == other.char_idx;
    }
  };

  void RebuildFromParagraphs();
  void FindKeywordMatches(const std::vector<std::string> &keywords);
  double ComputeTargetLineWidth(double em, double pad_x) const;
  double ComputeContentLeftX(double target_w) const;
  void InvalidateAllLayouts();
  typesetting::TypesetStyle BuildTypesetStyle() const;
  bool LayoutParagraphOnly(size_t idx, const class QFontMetricsF &fm, const typesetting::ITextMetrics &metrics,
                           double target_w, const typesetting::TypesetStyle &style);
  void EnsureParagraphsLaidOutUpTo(size_t last_idx);
  void RecomputePositionsFrom(size_t start_idx);
  void UpdateScrollBarRange();
  TextPos HitTest(const QPoint &viewport_pos) const;
  bool HasSelection() const { return sel_anchor_.IsValid() && sel_cursor_.IsValid() && !(sel_anchor_ == sel_cursor_); }
  void NormalizedSelection(TextPos *out_start, TextPos *out_end) const;

  typesetting::PreviewTypographyConfig cfg_;
  std::vector<ParagraphEntry> paragraphs_;
  std::vector<MatchLocation> matches_;
  int current_match_idx_ = -1;
  bool highlights_truncated_ = false;

  TextPos sel_anchor_;
  TextPos sel_cursor_;
  bool mouse_dragging_ = false;
  double total_doc_height_ = 0.0;
  double cached_target_w_ = 0.0;
  QTimer resize_debounce_;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_KNUTH_PLASS_VIEWER_H_
