// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/knuth_plass_viewer.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <algorithm>
#include <cmath>
#include <limits>

#include "gui/i18n.h"

namespace soeasy {
namespace gui {

namespace {

constexpr size_t kMaxKpHighlights = 100;
constexpr double kDocTopPadding = 16.0;
constexpr double kDocBottomPadding = 24.0;
constexpr double kMinHorizPadding = 16.0;

struct SpanRange {
  int start = 0;
  int end = 0;
  bool is_active = false;
};

template <typename BoxT>
bool IsAtomicGlyphBox(const BoxT &bx) {
  if (bx.src_end - bx.src_start <= 1) {
    return true;
  }
  // Treat a single UTF-16 surrogate pair glyph (Emoji / CJK Ext-B) as atomic.
  return bx.src_end - bx.src_start == 2 && bx.text.size() == 2 && bx.text.at(0).isHighSurrogate();
}

template <typename BoxT>
std::pair<double, double> ComputeBoxSubSpanX(const QFontMetricsF &fm, double left_x, const BoxT &bx,
                                             int ov_l, int ov_r) {
  double x0 = left_x + bx.x;
  double x1 = x0 + bx.width;
  if (!IsAtomicGlyphBox(bx) && (ov_l > bx.src_start || ov_r < bx.src_end)) {
    const QString prefix = bx.text.left(ov_l - bx.src_start);
    const QString covered = bx.text.mid(ov_l - bx.src_start, ov_r - ov_l);
    x0 = left_x + bx.x + fm.horizontalAdvance(prefix);
    x1 = x0 + fm.horizontalAdvance(covered);
  }
  return {x0, x1};
}

// Collect overlapping highlights/selection on a line and draw contiguous filled rectangles across inter-box glues.
void PaintLineHighlights(QPainter &p, const QRectF &line_band, double baseline_y, const QFontMetricsF &fm,
                         const std::vector<std::pair<double, double>> &x_spans, const QColor &fill_color,
                         bool rounded) {
  if (x_spans.empty()) return;
  p.save();
  p.setPen(Qt::NoPen);
  p.setBrush(fill_color);
  // Merge spans separated by small elastic gaps (<= 1.2 * em) on the same match
  const double merge_tol = fm.height() * 0.9;
  std::vector<std::pair<double, double>> merged = x_spans;
  std::sort(merged.begin(), merged.end());
  double cur_l = merged[0].first;
  double cur_r = merged[0].second;
  const double top = baseline_y - fm.ascent() - 1.0;
  const double h = fm.height() + 2.0;

  for (size_t i = 1; i < merged.size(); ++i) {
    if (merged[i].first <= cur_r + merge_tol) {
      cur_r = std::max(cur_r, merged[i].second);
    } else {
      QRectF rect(cur_l, top, std::max(1.0, cur_r - cur_l), h);
      rect = rect.intersected(line_band);
      if (rounded) p.drawRoundedRect(rect, 2.0, 2.0);
      else p.fillRect(rect, fill_color);
      cur_l = merged[i].first;
      cur_r = merged[i].second;
    }
  }
  QRectF rect(cur_l, top, std::max(1.0, cur_r - cur_l), h);
  rect = rect.intersected(line_band);
  if (rounded) p.drawRoundedRect(rect, 2.0, 2.0);
  else p.fillRect(rect, fill_color);
  p.restore();
}

}  // namespace

KnuthPlassViewer::KnuthPlassViewer(QWidget *parent) : QAbstractScrollArea(parent) {
  cfg_ = typesetting::PreviewTypographyStore::Instance().config();
  setFocusPolicy(Qt::StrongFocus);
  viewport()->setCursor(Qt::IBeamCursor);
  viewport()->setMouseTracking(true);
  setObjectName("kpViewerFrame");
  setStyleSheet(
      "#kpViewerFrame {"
      "  border: 1px solid #CBD5E1;"
      "  border-radius: 6px;"
      "}"
      "QScrollBar:vertical { width: 6px; background: transparent; margin: 0px; }"
      "QScrollBar::handle:vertical { background: #CBD5E1; min-height: 24px; border-radius: 3px; }"
      "QScrollBar::handle:vertical:hover { background: #94A3B8; }"
      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: transparent; }"
      "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
      "QScrollBar:horizontal { height: 6px; background: transparent; margin: 0px; }"
      "QScrollBar::handle:horizontal { background: #CBD5E1; min-width: 24px; border-radius: 3px; }"
      "QScrollBar::handle:horizontal:hover { background: #94A3B8; }"
      "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; background: transparent; }"
      "QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }");
  verticalScrollBar()->setSingleStep(28);

  resize_debounce_.setSingleShot(true);
  resize_debounce_.setInterval(40);
  connect(&resize_debounce_, &QTimer::timeout, this, [this]() {
    if (!paragraphs_.empty()) {
      const QFont font = typesetting::PreviewTypographyStore::Instance().ResolveFont(cfg_);
      const QFontMetricsF fm(font);
      const typesetting::QtTextMetrics metrics(fm);
      const double new_target_w = ComputeTargetLineWidth(metrics.EmSize(), kMinHorizPadding);
      if (std::abs(new_target_w - cached_target_w_) >= 0.5) {
        InvalidateAllLayouts();
      }
    }
    UpdateScrollBarRange();
    viewport()->update();
  });
}

void KnuthPlassViewer::SetTypographyConfig(const typesetting::PreviewTypographyConfig &cfg) {
  if (cfg_ == cfg) {
    return;
  }
  const bool need_relayout =
      cfg_.font_choice != cfg.font_choice || cfg_.font_size_px != cfg.font_size_px ||
      cfg_.auto_width != cfg.auto_width || std::abs(cfg_.text_width_em - cfg.text_width_em) > 1e-6 ||
      std::abs(cfg_.indent_em - cfg.indent_em) > 1e-6 ||
      std::abs(cfg_.par_skip_em - cfg.par_skip_em) > 1e-6 ||
      std::abs(cfg_.line_skip - cfg.line_skip) > 1e-6 || cfg_.hyphen_lang != cfg.hyphen_lang ||
      cfg_.runt_penalty != cfg.runt_penalty || cfg_.looseness != cfg.looseness;
  cfg_ = cfg;
  if (need_relayout) {
    InvalidateAllLayouts();
    UpdateScrollBarRange();
  }
  viewport()->update();
}

void KnuthPlassViewer::Clear() {
  paragraphs_.clear();
  matches_.clear();
  current_match_idx_ = -1;
  highlights_truncated_ = false;
  sel_anchor_ = TextPos{};
  sel_cursor_ = TextPos{};
  total_doc_height_ = 0.0;
  UpdateScrollBarRange();
  viewport()->update();
}

void KnuthPlassViewer::SetContent(const QString &text, const std::vector<std::string> &keywords) {
  paragraphs_.clear();
  matches_.clear();
  current_match_idx_ = -1;
  highlights_truncated_ = false;
  sel_anchor_ = TextPos{};
  sel_cursor_ = TextPos{};

  const QStringList raw_paras = typesetting::SplitParagraphs(text, true);
  paragraphs_.reserve(static_cast<size_t>(raw_paras.size()));
  for (const QString &raw : raw_paras) {
    ParagraphEntry entry;
    entry.raw_text = raw;
    entry.joined_text = typesetting::JoinLines(raw);
    if (!entry.joined_text.isEmpty()) {
      paragraphs_.push_back(std::move(entry));
    }
  }

  FindKeywordMatches(keywords);
  RebuildFromParagraphs();
  verticalScrollBar()->setValue(0);
  viewport()->update();
}

void KnuthPlassViewer::FindKeywordMatches(const std::vector<std::string> &keywords) {
  matches_.clear();
  highlights_truncated_ = false;
  if (keywords.empty() || paragraphs_.empty()) {
    return;
  }

  for (const auto &kw : keywords) {
    if (kw.empty()) continue;
    const QString needle = QString::fromStdString(kw);
    for (size_t pi = 0; pi < paragraphs_.size(); ++pi) {
      const QString &hay = paragraphs_[pi].joined_text;
      int pos = 0;
      while ((pos = hay.indexOf(needle, pos, Qt::CaseInsensitive)) != -1) {
        matches_.push_back({pi, pos, pos + needle.size()});
        if (matches_.size() >= kMaxKpHighlights) {
          highlights_truncated_ = true;
          break;
        }
        pos += std::max(1, needle.size());
      }
      if (highlights_truncated_) break;
    }
    if (highlights_truncated_) break;
  }

  if (!matches_.empty()) {
    std::sort(matches_.begin(), matches_.end(), [](const MatchLocation &a, const MatchLocation &b) {
      if (a.para_idx != b.para_idx) return a.para_idx < b.para_idx;
      if (a.start != b.start) return a.start < b.start;
      return a.end < b.end;
    });
    matches_.erase(std::unique(matches_.begin(), matches_.end(),
                               [](const MatchLocation &a, const MatchLocation &b) {
                                 return a.para_idx == b.para_idx && a.start == b.start && a.end == b.end;
                               }),
                   matches_.end());
    current_match_idx_ = 0;
  }
}

double KnuthPlassViewer::ComputeTargetLineWidth(double em, double pad_x) const {
  const double avail = std::max(140.0, static_cast<double>(viewport()->width()) - pad_x * 2.0);
  const double desired = std::max(8.0, cfg_.text_width_em) * em;
  if (cfg_.auto_width) {
    return std::min(avail, desired);
  }
  return desired;
}

double KnuthPlassViewer::ComputeContentLeftX(double target_w) const {
  const double vw = static_cast<double>(viewport()->width());
  const double scroll_x = static_cast<double>(horizontalScrollBar()->value());
  return std::max(kMinHorizPadding, (vw - target_w) * 0.5) - scroll_x;
}

void KnuthPlassViewer::RebuildFromParagraphs() {
  InvalidateAllLayouts();
  // Eagerly lay out initial screen buffer so initial view & scrollbar are immediately exact
  if (!paragraphs_.empty()) {
    const size_t last_eager = std::min<size_t>(paragraphs_.size(), 30) - 1;
    EnsureParagraphsLaidOutUpTo(last_eager);
  } else {
    UpdateScrollBarRange();
  }
}

void KnuthPlassViewer::InvalidateAllLayouts() {
  const QFont font = typesetting::PreviewTypographyStore::Instance().ResolveFont(cfg_);
  const QFontMetricsF fm(font);
  const typesetting::QtTextMetrics metrics(fm);
  const double em = metrics.EmSize();
  const double target_w = ComputeTargetLineWidth(em, kMinHorizPadding);
  cached_target_w_ = target_w;

  const double chars_per_line = std::max(8.0, target_w / std::max(6.0, em));
  const double line_h = static_cast<double>(cfg_.font_size_px) * cfg_.line_skip;
  const double par_skip = static_cast<double>(cfg_.font_size_px) * cfg_.par_skip_em;

  double cur_y = kDocTopPadding;
  for (size_t i = 0; i < paragraphs_.size(); ++i) {
    ParagraphEntry &entry = paragraphs_[i];
    entry.laid_out = false;
    entry.lines.clear();
    entry.laid_width = -1.0;
    entry.y_top = cur_y;
    const int est_lines = std::max(1, static_cast<int>(std::ceil(entry.joined_text.size() / chars_per_line)));
    entry.height = static_cast<double>(est_lines) * line_h;
    cur_y += entry.height + par_skip;
  }
  total_doc_height_ = paragraphs_.empty() ? 0.0 : (cur_y - par_skip + kDocBottomPadding);
}

bool KnuthPlassViewer::LayoutParagraphOnly(size_t idx, const QFontMetricsF &fm,
                                           const typesetting::ITextMetrics &metrics, double target_w,
                                           const typesetting::TypesetStyle &style) {
  if (idx >= paragraphs_.size()) return false;
  ParagraphEntry &entry = paragraphs_[idx];

  if (entry.laid_out && std::abs(entry.laid_width - target_w) < 0.5) {
    return false;
  }

  const auto tp = typesetting::BreakParagraph3Pass(entry.raw_text, style, metrics, {target_w});

  const double line_h = static_cast<double>(cfg_.font_size_px) * cfg_.line_skip;
  const double ascent_offset = fm.ascent() + static_cast<double>(cfg_.font_size_px) * (cfg_.line_skip - 1.0) * 0.5;

  entry.lines.clear();
  entry.lines.reserve(tp.lines.size());

  for (size_t li = 0; li < tp.lines.size(); ++li) {
    const auto &ln = tp.lines[li];
    const bool is_last_line = (li + 1 == tp.lines.size());
    double r = std::max(ln.ratio, -1.0);
    if (!cfg_.justify || is_last_line) {
      r = std::min(r, 0.0);
    }

    RenderedLine rline;
    rline.y_baseline = static_cast<double>(li) * line_h + ascent_offset;
    double x = 0.0;

    for (int k = ln.start; k < ln.end; ++k) {
      const auto &it = tp.items[k];
      if (it.kind == typesetting::ItemKind::kBox) {
        RenderedBox rb{x, it.width, it.draw_dx, it.text, it.src_start, it.src_end};
        if (it.src_start >= 0 && it.src_end > it.src_start) {
          if (rline.src_start < 0) rline.src_start = it.src_start;
          rline.src_end = it.src_end;
        }
        rline.boxes.push_back(std::move(rb));
        x += it.width;
      } else if (it.kind == typesetting::ItemKind::kGlue) {
        x += std::max(0.0, it.GetGlueWidth(r));
      }
    }

    if (ln.end < static_cast<int>(tp.items.size()) &&
        tp.items[ln.end].kind == typesetting::ItemKind::kPenalty &&
        !tp.items[ln.end].text.isEmpty()) {
      rline.trailing_hyphen = tp.items[ln.end].text;
      rline.hyphen_x = x;
    }
    rline.line_end_x = x;
    entry.lines.push_back(std::move(rline));
  }

  const double old_h = entry.height;
  const double new_h = std::max(1, static_cast<int>(entry.lines.size())) * line_h;
  entry.height = new_h;
  entry.laid_width = target_w;
  entry.laid_out = true;

  return std::abs(new_h - old_h) > 0.5;
}

typesetting::TypesetStyle KnuthPlassViewer::BuildTypesetStyle() const {
  typesetting::TypesetStyle style;
  style.size = static_cast<double>(cfg_.font_size_px);
  style.hyphen_lang = cfg_.hyphen_lang;
  style.indent_em = cfg_.indent_em;
  style.runt_penalty = cfg_.runt_penalty;
  style.looseness = cfg_.looseness;
  style.hyphenator = typesetting::PreviewTypographyStore::Instance().GetHyphenator(cfg_.hyphen_lang);
  return style;
}

void KnuthPlassViewer::EnsureParagraphsLaidOutUpTo(size_t last_idx) {
  if (paragraphs_.empty()) {
    UpdateScrollBarRange();
    return;
  }
  const size_t bounded_last = std::min(last_idx, paragraphs_.size() - 1);

  const QFont font = typesetting::PreviewTypographyStore::Instance().ResolveFont(cfg_);
  const QFontMetricsF fm(font);
  const typesetting::QtTextMetrics metrics(fm);
  const double target_w = ComputeTargetLineWidth(metrics.EmSize(), kMinHorizPadding);
  cached_target_w_ = target_w;
  const typesetting::TypesetStyle style = BuildTypesetStyle();

  size_t first_changed = paragraphs_.size();
  for (size_t i = 0; i <= bounded_last; ++i) {
    if (LayoutParagraphOnly(i, fm, metrics, target_w, style) && first_changed == paragraphs_.size()) {
      first_changed = i;
    }
  }

  if (first_changed < paragraphs_.size()) {
    RecomputePositionsFrom(first_changed + 1);
  } else {
    UpdateScrollBarRange();
  }
}

void KnuthPlassViewer::RecomputePositionsFrom(size_t start_idx) {
  const double par_skip = static_cast<double>(cfg_.font_size_px) * cfg_.par_skip_em;
  double cur_y = kDocTopPadding;
  if (start_idx > 0 && start_idx <= paragraphs_.size()) {
    const auto &prev = paragraphs_[start_idx - 1];
    cur_y = prev.y_top + prev.height + par_skip;
  }
  for (size_t i = start_idx; i < paragraphs_.size(); ++i) {
    paragraphs_[i].y_top = cur_y;
    cur_y += paragraphs_[i].height + par_skip;
  }
  total_doc_height_ = paragraphs_.empty() ? 0.0 : (cur_y - par_skip + kDocBottomPadding);
  UpdateScrollBarRange();
}

void KnuthPlassViewer::UpdateScrollBarRange() {
  const int vh = viewport()->height();
  const int max_v_scroll = std::max(0, static_cast<int>(std::ceil(total_doc_height_)) - vh);
  verticalScrollBar()->setPageStep(vh);
  verticalScrollBar()->setRange(0, max_v_scroll);

  const int vw = viewport()->width();
  const int content_w = static_cast<int>(std::ceil(cached_target_w_ + kMinHorizPadding * 2.0));
  const int max_h_scroll = std::max(0, content_w - vw);
  horizontalScrollBar()->setPageStep(vw);
  horizontalScrollBar()->setRange(0, max_h_scroll);
}

void KnuthPlassViewer::JumpToMatch(int index) {
  if (matches_.empty()) {
    current_match_idx_ = -1;
    viewport()->update();
    return;
  }
  if (index < 0) index = static_cast<int>(matches_.size()) - 1;
  else if (index >= static_cast<int>(matches_.size())) index = 0;

  current_match_idx_ = index;
  const MatchLocation &m = matches_[static_cast<size_t>(current_match_idx_)];

  // Batch-lay out paragraphs up to target without cascading per-paragraph position shifts
  EnsureParagraphsLaidOutUpTo(m.para_idx);

  const auto &para = paragraphs_[m.para_idx];
  double target_y = para.y_top;
  for (const auto &ln : para.lines) {
    if (ln.src_end >= m.start && ln.src_start <= m.end) {
      target_y = para.y_top + ln.y_baseline;
      break;
    }
  }

  const int vh = viewport()->height();
  const int scroll_y = std::clamp(static_cast<int>(target_y - vh * 0.35), 0, verticalScrollBar()->maximum());
  verticalScrollBar()->setValue(scroll_y);
  viewport()->update();
}

size_t KnuthPlassViewer::LaidOutParagraphCount() const {
  size_t count = 0;
  for (const auto &p : paragraphs_) {
    if (p.laid_out) ++count;
  }
  return count;
}

void KnuthPlassViewer::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  resize_debounce_.start();
}

void KnuthPlassViewer::paintEvent(QPaintEvent *event) {
  QPainter p(viewport());
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::TextAntialiasing);
  p.fillRect(viewport()->rect(), cfg_.bg_color);

  if (paragraphs_.empty()) {
    return;
  }

  const QFont font = typesetting::PreviewTypographyStore::Instance().ResolveFont(cfg_);
  p.setFont(font);
  const QFontMetricsF fm(font);

  const double scroll_y = static_cast<double>(verticalScrollBar()->value());
  const double vis_top = scroll_y + static_cast<double>(event->rect().top()) - 120.0;
  const double vis_bottom = scroll_y + static_cast<double>(event->rect().bottom()) + 200.0;

  // Locate first visible paragraph via binary search on y_top
  auto it_first = std::lower_bound(
      paragraphs_.begin(), paragraphs_.end(), vis_top,
      [](const ParagraphEntry &e, double y) { return (e.y_top + e.height) < y; });
  size_t first_idx = static_cast<size_t>(std::distance(paragraphs_.begin(), it_first));
  if (first_idx > 2) first_idx -= 2;

  const typesetting::QtTextMetrics metrics(fm);
  const double target_w = ComputeTargetLineWidth(metrics.EmSize(), kMinHorizPadding);
  cached_target_w_ = target_w;
  const typesetting::TypesetStyle style = BuildTypesetStyle();

  // Loop until all paragraphs pulled into [first_idx, vis_bottom] (even after height shrinkage) are laid out.
  for (size_t cursor = first_idx;;) {
    size_t first_changed = paragraphs_.size();
    size_t next_unseen = cursor;
    for (; next_unseen < paragraphs_.size() && paragraphs_[next_unseen].y_top <= vis_bottom; ++next_unseen) {
      if (LayoutParagraphOnly(next_unseen, fm, metrics, target_w, style) &&
          first_changed == paragraphs_.size()) {
        first_changed = next_unseen;
      }
    }
    if (first_changed >= paragraphs_.size()) {
      break;
    }
    RecomputePositionsFrom(first_changed + 1);
    cursor = next_unseen;
  }

  const double left_x = ComputeContentLeftX(cached_target_w_);

  if (cfg_.show_guides) {
    p.save();
    p.setPen(QPen(QColor("#E0A0A0"), 0.8));
    p.drawLine(QPointF(left_x, 0.0), QPointF(left_x, static_cast<double>(viewport()->height())));
    p.drawLine(QPointF(left_x + cached_target_w_, 0.0),
               QPointF(left_x + cached_target_w_, static_cast<double>(viewport()->height())));
    p.restore();
  }

  const bool dark_bg = cfg_.bg_color.lightness() < 128;
  const QColor norm_hl_bg = dark_bg ? QColor("#B45309") : QColor("#FEF08A");
  const QColor norm_hl_fg = dark_bg ? QColor("#FEF3C7") : QColor("#854D0E");
  const QColor act_hl_bg("#EA580C");
  const QColor act_hl_fg("#FFFFFF");
  const QColor sel_bg = dark_bg ? QColor("#1D4ED8") : QColor("#BFDBFE");

  TextPos sel_start, sel_end;
  const bool has_sel = HasSelection();
  if (has_sel) {
    NormalizedSelection(&sel_start, &sel_end);
  }

  const QRectF clip_band(0.0, 0.0, static_cast<double>(viewport()->width()),
                         static_cast<double>(viewport()->height()));

  for (size_t pi = first_idx; pi < paragraphs_.size(); ++pi) {
    const ParagraphEntry &para = paragraphs_[pi];
    if (para.y_top > vis_bottom) break;
    if (para.y_top + para.height < vis_top) continue;

    // Collect keyword matches belonging to this paragraph
    std::vector<SpanRange> para_matches;
    for (size_t mi = 0; mi < matches_.size(); ++mi) {
      if (matches_[mi].para_idx == pi) {
        para_matches.push_back(
            {matches_[mi].start, matches_[mi].end, static_cast<int>(mi) == current_match_idx_});
      }
    }

    for (const auto &ln : para.lines) {
      const double base_y = para.y_top + ln.y_baseline - scroll_y;
      if (base_y + fm.descent() < 0.0 || base_y - fm.ascent() > clip_band.bottom()) {
        continue;
      }

      // 1. Paint selection background first (if overlapping)
      if (has_sel && static_cast<int>(pi) >= sel_start.para_idx &&
          static_cast<int>(pi) <= sel_end.para_idx && ln.src_start >= 0) {
        const int line_sel_l = (static_cast<int>(pi) == sel_start.para_idx) ? sel_start.char_idx : 0;
        const int line_sel_r = (static_cast<int>(pi) == sel_end.para_idx)
                                   ? sel_end.char_idx
                                   : para.joined_text.size();
        if (line_sel_r > line_sel_l) {
          std::vector<std::pair<double, double>> sel_spans;
          for (const auto &bx : ln.boxes) {
            if (bx.src_start < 0 || bx.src_end <= line_sel_l || bx.src_start >= line_sel_r) continue;
            const int ov_l = std::max(bx.src_start, line_sel_l);
            const int ov_r = std::min(bx.src_end, line_sel_r);
            sel_spans.push_back(ComputeBoxSubSpanX(fm, left_x, bx, ov_l, ov_r));
          }
          PaintLineHighlights(p, clip_band, base_y, fm, sel_spans, sel_bg, false);
        }
      }

      // 2. Paint merged normal & active keyword highlights
      if (!para_matches.empty()) {
        std::vector<std::pair<double, double>> norm_spans;
        std::vector<std::pair<double, double>> act_spans;
        for (const auto &bx : ln.boxes) {
          if (bx.src_start < 0) continue;
          for (const auto &m : para_matches) {
            if (bx.src_end <= m.start || bx.src_start >= m.end) continue;
            const int ov_l = std::max(bx.src_start, m.start);
            const int ov_r = std::min(bx.src_end, m.end);
            const auto span_x = ComputeBoxSubSpanX(fm, left_x, bx, ov_l, ov_r);
            if (m.is_active) act_spans.push_back(span_x);
            else norm_spans.push_back(span_x);
          }
        }
        PaintLineHighlights(p, clip_band, base_y, fm, norm_spans, norm_hl_bg, true);
        PaintLineHighlights(p, clip_band, base_y, fm, act_spans, act_hl_bg, true);
      }

      // 3. Draw glyph boxes cleanly on top
      for (const auto &bx : ln.boxes) {
        bool in_active = false;
        bool in_normal = false;
        if (bx.src_start >= 0 && !para_matches.empty()) {
          for (const auto &m : para_matches) {
            if (bx.src_end > m.start && bx.src_start < m.end) {
              if (m.is_active) in_active = true;
              else in_normal = true;
            }
          }
        }
        p.setPen(in_active ? act_hl_fg : (in_normal ? norm_hl_fg : cfg_.fg_color));
        p.drawText(QPointF(left_x + bx.x + bx.draw_dx, base_y), bx.text);
      }

      if (!ln.trailing_hyphen.isEmpty()) {
        p.setPen(cfg_.fg_color);
        p.drawText(QPointF(left_x + ln.hyphen_x, base_y), ln.trailing_hyphen);
      }
    }
  }
}

KnuthPlassViewer::TextPos KnuthPlassViewer::HitTest(const QPoint &viewport_pos) const {
  if (paragraphs_.empty()) return {};
  const double doc_y = static_cast<double>(viewport_pos.y()) + static_cast<double>(verticalScrollBar()->value());
  const double left_x = ComputeContentLeftX(cached_target_w_);
  const double rel_x = static_cast<double>(viewport_pos.x()) - left_x;

  auto it_para = std::lower_bound(
      paragraphs_.begin(), paragraphs_.end(), doc_y,
      [](const ParagraphEntry &e, double y) { return (e.y_top + e.height) <= y; });
  size_t pi = (it_para == paragraphs_.end())
                  ? paragraphs_.size() - 1
                  : static_cast<size_t>(std::distance(paragraphs_.begin(), it_para));
  const ParagraphEntry &para = paragraphs_[pi];
  if (para.lines.empty()) {
    return {static_cast<int>(pi), 0};
  }

  const QFont font = typesetting::PreviewTypographyStore::Instance().ResolveFont(cfg_);
  const QFontMetricsF fm(font);

  int line_idx = 0;
  double best_dy = std::numeric_limits<double>::max();
  for (size_t li = 0; li < para.lines.size(); ++li) {
    const double mid_y = para.y_top + para.lines[li].y_baseline - fm.ascent() * 0.35;
    const double dy = std::abs(doc_y - mid_y);
    if (dy < best_dy) {
      best_dy = dy;
      line_idx = static_cast<int>(li);
    }
  }

  const RenderedLine &ln = para.lines[static_cast<size_t>(line_idx)];
  if (ln.src_start < 0) return {static_cast<int>(pi), 0};
  if (rel_x <= 0.0) return {static_cast<int>(pi), ln.src_start};
  if (rel_x >= ln.line_end_x) return {static_cast<int>(pi), ln.src_end};

  for (const auto &bx : ln.boxes) {
    if (bx.src_start < 0) continue;
    if (rel_x <= bx.x + bx.width) {
      if (IsAtomicGlyphBox(bx)) {
        return {static_cast<int>(pi), (rel_x < bx.x + bx.width * 0.5) ? bx.src_start : bx.src_end};
      }
      // Sub-box hit testing inside Latin words
      const int len = bx.src_end - bx.src_start;
      for (int c = 1; c <= len; ++c) {
        const double w = fm.horizontalAdvance(bx.text.left(c));
        if (rel_x < bx.x + w) {
          const double prev_w = fm.horizontalAdvance(bx.text.left(c - 1));
          return {static_cast<int>(pi), bx.src_start + ((rel_x - (bx.x + prev_w)) < (w - prev_w) * 0.5 ? c - 1 : c)};
        }
      }
      return {static_cast<int>(pi), bx.src_end};
    }
  }
  return {static_cast<int>(pi), ln.src_end};
}

void KnuthPlassViewer::NormalizedSelection(TextPos *out_start, TextPos *out_end) const {
  if (sel_anchor_ < sel_cursor_) {
    *out_start = sel_anchor_;
    *out_end = sel_cursor_;
  } else {
    *out_start = sel_cursor_;
    *out_end = sel_anchor_;
  }
}

QString KnuthPlassViewer::SelectedText() const {
  if (!HasSelection() || paragraphs_.empty()) {
    return {};
  }
  TextPos s, e;
  NormalizedSelection(&s, &e);
  QString out;
  for (int pi = s.para_idx; pi <= e.para_idx && pi < static_cast<int>(paragraphs_.size()); ++pi) {
    const QString &jtxt = paragraphs_[static_cast<size_t>(pi)].joined_text;
    const int from = (pi == s.para_idx) ? std::clamp(s.char_idx, 0, jtxt.size()) : 0;
    const int to = (pi == e.para_idx) ? std::clamp(e.char_idx, 0, jtxt.size()) : jtxt.size();
    if (!out.isEmpty()) out.append("\n\n");
    out.append(jtxt.mid(from, std::max(0, to - from)));
  }
  return out;
}

QString KnuthPlassViewer::PlainJoinedText() const {
  QString out;
  for (size_t i = 0; i < paragraphs_.size(); ++i) {
    if (i > 0) out.append("\n\n");
    out.append(paragraphs_[i].joined_text);
  }
  return out;
}

void KnuthPlassViewer::SelectAllText() {
  if (paragraphs_.empty()) return;
  sel_anchor_ = {0, 0};
  const int last_p = static_cast<int>(paragraphs_.size()) - 1;
  sel_cursor_ = {last_p, paragraphs_.back().joined_text.size()};
  viewport()->update();
}

void KnuthPlassViewer::CopySelection() const {
  const QString txt = HasSelection() ? SelectedText() : QString();
  if (!txt.isEmpty() && QApplication::clipboard()) {
    QApplication::clipboard()->setText(txt);
  }
}

void KnuthPlassViewer::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    setFocus();
    const TextPos hit = HitTest(event->pos());
    if (event->modifiers() & Qt::ShiftModifier && sel_anchor_.IsValid()) {
      sel_cursor_ = hit;
    } else {
      sel_anchor_ = hit;
      sel_cursor_ = hit;
      mouse_dragging_ = true;
    }
    viewport()->update();
  }
  QAbstractScrollArea::mousePressEvent(event);
}

void KnuthPlassViewer::mouseMoveEvent(QMouseEvent *event) {
  if (mouse_dragging_) {
    sel_cursor_ = HitTest(event->pos());
    viewport()->update();
  }
  QAbstractScrollArea::mouseMoveEvent(event);
}

void KnuthPlassViewer::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    mouse_dragging_ = false;
  }
  QAbstractScrollArea::mouseReleaseEvent(event);
}

void KnuthPlassViewer::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    const TextPos hit = HitTest(event->pos());
    if (hit.IsValid() && static_cast<size_t>(hit.para_idx) < paragraphs_.size()) {
      sel_anchor_ = {hit.para_idx, 0};
      sel_cursor_ = {hit.para_idx, paragraphs_[static_cast<size_t>(hit.para_idx)].joined_text.size()};
      viewport()->update();
    }
  }
  QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void KnuthPlassViewer::keyPressEvent(QKeyEvent *event) {
  if (event->matches(QKeySequence::Copy)) {
    CopySelection();
    event->accept();
    return;
  }
  if (event->matches(QKeySequence::SelectAll)) {
    SelectAllText();
    event->accept();
    return;
  }
  QAbstractScrollArea::keyPressEvent(event);
}

void KnuthPlassViewer::contextMenuEvent(QContextMenuEvent *event) {
  QMenu menu(this);
  QAction *act_copy = menu.addAction(Tr("ctx_copy"));
  act_copy->setEnabled(HasSelection());
  connect(act_copy, &QAction::triggered, this, &KnuthPlassViewer::CopySelection);

  QAction *act_all = menu.addAction(Tr("ctx_select_all"));
  act_all->setEnabled(!paragraphs_.empty());
  connect(act_all, &QAction::triggered, this, &KnuthPlassViewer::SelectAllText);

  menu.exec(event->globalPos());
}

}  // namespace gui
}  // namespace soeasy
