// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_GUI_TYPESETTING_CJK_TYPESETTER_H_
#define SOEASY_SRC_GUI_TYPESETTING_CJK_TYPESETTER_H_

#include <QFontMetricsF>
#include <QString>
#include <QStringList>
#include <memory>
#include <string>
#include <vector>

#include "gui/typesetting/kp_types.h"

namespace soeasy {
namespace gui {
namespace typesetting {

struct GlyphTrim {
  double width = 0.0;
  double draw_dx = 0.0;
};

/**
 * @brief Abstraction over font metrics so unit tests can run deterministically
 * without system fonts while GUI uses Qt5 QFontMetricsF.
 */
class ITextMetrics {
 public:
  virtual ~ITextMetrics() = default;
  virtual double EmSize() const = 0;
  virtual double MeasureAdvance(const QString &text) const = 0;
  virtual GlyphTrim MeasurePunctTrim(const QString &ch, bool is_open_punct) const = 0;
};

class QtTextMetrics : public ITextMetrics {
 public:
  explicit QtTextMetrics(const QFontMetricsF &fm);
  double EmSize() const override;
  double MeasureAdvance(const QString &text) const override;
  GlyphTrim MeasurePunctTrim(const QString &ch, bool is_open_punct) const override;

 private:
  QFontMetricsF fm_;
  double em_ = 14.0;
};

/**
 * @brief RAII wrapper around hunspell/hyphen for Pass-2 Latin hyphenation.
 */
class Hyphenator {
 public:
  Hyphenator() = default;
  ~Hyphenator();
  Hyphenator(const Hyphenator &) = delete;
  Hyphenator &operator=(const Hyphenator &) = delete;

  bool LoadFromFile(const QString &dic_path);
  bool LoadFromData(const char *data, size_t len);
  bool IsLoaded() const { return dict_ != nullptr; }

  // Returns hyphenation split indices into `word` obeying \lefthyphenmin=2 \righthyphenmin=3.
  std::vector<int> FindBreakPositions(const QString &word) const;

 private:
  struct Impl;
  void *dict_ = nullptr;
};

struct TypesetStyle {
  double size = 14.0;
  QString hyphen_lang = "en_US";
  double indent_em = 0.0;
  double cjk_stretch_em = 1.0 / 8.0;
  double cjk_shrink_em = 0.0;
  double punct_stretch_em = 1.0 / 3.0;
  double punct_shrink_em = 1.0 / 4.0;
  double autospace_em = 1.0 / 4.0;
  int runt_penalty = 80;
  int looseness = 0;
  int line_penalty = kDefaultLinePenalty;
  bool kp81_penalty = false;
  const Hyphenator *hyphenator = nullptr;
};

struct Piece {
  QString text;
  bool is_latin = false;
  bool has_trailing_space = false;
  bool allow_break_after = true;
  int src_start = -1;
  int src_end = -1;
};

struct TypesetParagraph {
  QString joined_text;
  std::vector<Item> items;
  std::vector<LineBreakResult> lines;
};

// Paragraph splitting & preprocessing matching chenshuo/typeset/linebreak/typeset.py
QStringList SplitParagraphs(const QString &text, bool preserve_structured_lines = false);
QString JoinLines(const QString &paragraph);
std::vector<QString> Uax14LineBreakUnits(const QString &text);
std::vector<Piece> SplitUnit(const QString &unit, int base_offset = 0);

bool IsWideCodePoint(uint32_t cp);
bool IsOpenPunct(QChar ch);
bool IsClosePunct(QChar ch);
bool IsStopPunct(QChar ch);
bool IsNoBreakBefore(QChar ch);
bool IsCjkPunct(QChar ch);

std::vector<Item> BuildParagraphItems(const QString &joined_text, const TypesetStyle &style,
                                      const ITextMetrics &metrics, bool enable_hyphenation);

TypesetParagraph BreakParagraph3Pass(const QString &raw_paragraph, const TypesetStyle &style,
                                     const ITextMetrics &metrics, const std::vector<double> &line_widths);

}  // namespace typesetting
}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_TYPESETTING_CJK_TYPESETTER_H_
