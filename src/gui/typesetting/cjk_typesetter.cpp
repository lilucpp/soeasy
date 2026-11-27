// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "gui/typesetting/cjk_typesetter.h"

#include <QByteArray>
#include <QFile>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <mutex>

#include "gui/typesetting/kp_engine.h"

extern "C" {
#include "eastasianwidthdef.h"
#include "hyphen.h"
#include "linebreak.h"
}

namespace soeasy {
namespace gui {
namespace typesetting {

namespace {

void EnsureLibunibreakInit() {
  static std::once_flag flag;
  std::call_once(flag, []() { init_linebreak(); });
}

uint32_t CodePointAt(const QString &s, int idx, int *step) {
  if (idx + 1 < s.size() && s[idx].isHighSurrogate() && s[idx + 1].isLowSurrogate()) {
    if (step) *step = 2;
    return QChar::surrogateToUcs4(s[idx], s[idx + 1]);
  }
  if (step) *step = 1;
  return static_cast<uint32_t>(s[idx].unicode());
}

bool IsCjkCharInUnit(uint32_t cp, QChar bmp_ch) {
  if (cp <= 0xFFFF && IsCjkPunct(bmp_ch)) {
    return true;
  }
  if (IsWideCodePoint(cp)) {
    return true;
  }
  if (cp <= 0xFFFF && ub_get_char_eaw_class(cp) == EAW_A && bmp_ch.isPunct()) {
    return true;
  }
  return false;
}

bool IsAllWideFalse(const QString &word) {
  for (int i = 0; i < word.size();) {
    int step = 1;
    uint32_t cp = CodePointAt(word, i, &step);
    if (IsWideCodePoint(cp)) {
      return false;
    }
    i += step;
  }
  return true;
}

// Ports typeset.py::may_break_at_quote()
bool MayBreakAtQuote(const Piece &prev, QChar ch) {
  if (prev.is_latin || prev.text.isEmpty()) {
    return false;
  }
  const QChar last = prev.text[prev.text.size() - 1];
  if (ch == QChar(0x2018) || ch == QChar(0x201C)) {
    return !IsOpenPunct(last);
  }
  if (last == QChar(0x2019) || last == QChar(0x201D)) {
    const uint32_t cp = static_cast<uint32_t>(ch.unicode());
    return !IsNoBreakBefore(ch) && IsWideCodePoint(cp);
  }
  return false;
}

int FindRuntBreakIndex(const std::vector<Piece> &pieces) {
  for (int i = static_cast<int>(pieces.size()) - 2; i >= 0; --i) {
    if (pieces[i].allow_break_after) {
      return i;
    }
  }
  return -1;
}

bool LooksLikeStructuredLine(const QString &trimmed_line) {
  if (trimmed_line.isEmpty()) return false;
  // Markdown headings, blockquotes, horizontal rules
  if (trimmed_line.startsWith("# ") || trimmed_line.startsWith("## ") ||
      trimmed_line.startsWith("### ") || trimmed_line.startsWith("> ")) {
    return true;
  }
  // Bullet lists ("- ", "* ", "• ", "· ")
  if (trimmed_line.startsWith("- ") || trimmed_line.startsWith("* ") ||
      trimmed_line.startsWith(QString::fromUtf8("• ")) || trimmed_line.startsWith(QString::fromUtf8("· "))) {
    return true;
  }
  // Ordered lists & Chinese/legal section markers:
  // "1. ", "2) ", "(1) ", "（1）", "（一）", "一、", "第一条..."
  static const QRegularExpression kOrderedRe(QString::fromUtf8(
      R"(^(?:\d+[.)、]|[（(](?:[一二三四五六七八九十百零]+|\d+)[）)]|[一二三四五六七八九十百]+[、.]|第[一二三四五六七八九十百零千\d]+[条章节款项篇编]))"));
  if (kOrderedRe.match(trimmed_line).hasMatch()) {
    return true;
  }
  // Divider lines ("---", "===")
  if (trimmed_line.startsWith("---") || trimmed_line.startsWith("===")) {
    return true;
  }
  // Chinese/English bracketed standalone short titles / publication notes like "【标题】", "（2021年...）"
  if (trimmed_line.size() <= 64 &&
      ((trimmed_line.startsWith(QString::fromUtf8("【")) && trimmed_line.endsWith(QString::fromUtf8("】"))) ||
       (trimmed_line.startsWith(QString::fromUtf8("（")) && trimmed_line.endsWith(QString::fromUtf8("）"))) ||
       (trimmed_line.startsWith('[') && trimmed_line.endsWith(']')))) {
    return true;
  }
  return false;
}

bool LineHasIndentPrefix(const QString &raw_line) {
  if (raw_line.startsWith('\t') || raw_line.startsWith(QChar(0x3000))) {
    return true;
  }
  return raw_line.startsWith("  ");
}

bool EndsWithParagraphTerminal(const QString &tline) {
  if (tline.isEmpty()) {
    return false;
  }
  static const QString kTerm = QString::fromUtf8("。！？；：）】》」』”’!?;:)");
  return kTerm.contains(tline.at(tline.size() - 1));
}

bool IsStandaloneHeadingLine(const QString &tline) {
  if (tline.startsWith("#") || tline.startsWith("---") || tline.startsWith("===")) {
    return true;
  }
  if (tline.size() <= 64 &&
      ((tline.startsWith(QString::fromUtf8("【")) && tline.endsWith(QString::fromUtf8("】"))) ||
       (tline.startsWith(QString::fromUtf8("（")) && tline.endsWith(QString::fromUtf8("）")) &&
        !tline.contains(QString::fromUtf8("，")) && !tline.contains(QString::fromUtf8("；"))))) {
    return true;
  }
  static const QRegularExpression kCnSectionRe(
      QString::fromUtf8(R"(^(?:[一二三四五六七八九十]+[、.]|第[一二三四五六七八九十百\d]+[章节编]))"));
  if (tline.size() <= 36 && kCnSectionRe.match(tline).hasMatch()) {
    const QChar last = tline.at(tline.size() - 1);
    return !IsStopPunct(last) && last != '.';
  }
  return false;
}

}  // namespace

QtTextMetrics::QtTextMetrics(const QFontMetricsF &fm) : fm_(fm) {
  const double han = fm_.horizontalAdvance(QString::fromUtf8("汉"));
  em_ = (han > 1.0) ? han : fm_.height();
}

double QtTextMetrics::EmSize() const { return em_; }

double QtTextMetrics::MeasureAdvance(const QString &text) const { return fm_.horizontalAdvance(text); }

GlyphTrim QtTextMetrics::MeasurePunctTrim(const QString &ch, bool is_open_punct) const {
  const double adv = fm_.horizontalAdvance(ch);
  const QRectF tight = fm_.tightBoundingRect(ch);
  if (tight.isEmpty() || adv <= 1e-6) {
    return {adv * 0.5, is_open_punct ? -adv * 0.5 : 0.0};
  }
  if (is_open_punct) {
    const double trim = std::max(0.0, std::min(adv * 0.48, tight.left() - adv * 0.04));
    return {adv - trim, -trim};
  }
  const double right_blank = std::max(0.0, adv - tight.right() - adv * 0.04);
  const double trim = std::min(adv * 0.48, right_blank);
  return {adv - trim, 0.0};
}

Hyphenator::~Hyphenator() {
  if (dict_) {
    hnj_hyphen_free(static_cast<HyphenDict *>(dict_));
    dict_ = nullptr;
  }
}

bool Hyphenator::LoadFromFile(const QString &dic_path) {
  QFile f(dic_path);
  if (!f.open(QIODevice::ReadOnly)) {
    return false;
  }
  const QByteArray data = f.readAll();
  return LoadFromData(data.constData(), static_cast<size_t>(data.size()));
}

bool Hyphenator::LoadFromData(const char *data, size_t len) {
  if (dict_) {
    hnj_hyphen_free(static_cast<HyphenDict *>(dict_));
    dict_ = nullptr;
  }
  if (!data || len == 0) {
    return false;
  }
  dict_ = hnj_hyphen_load_data(data, len);
  return dict_ != nullptr;
}

std::vector<int> Hyphenator::FindBreakPositions(const QString &word) const {
  std::vector<int> positions;
  if (!dict_ || word.size() < 5) {
    return positions;
  }
  for (QChar c : word) {
    const ushort u = c.unicode();
    if (!((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z'))) {
      return positions;
    }
  }
  const QByteArray lower = word.toLower().toUtf8();
  const int n = lower.size();
  std::vector<char> hyphens(static_cast<size_t>(n) + 6, 0);
  if (hnj_hyphen_hyphenate2(static_cast<HyphenDict *>(dict_), lower.constData(), n, hyphens.data(), nullptr,
                            nullptr, nullptr, nullptr) != 0) {
    return positions;
  }
  for (int i = 0; i < n - 1; ++i) {
    if ((hyphens[i] & 1) != 0) {
      const int pos = i + 1;
      if (pos >= 2 && pos <= n - 3) {
        positions.push_back(pos);
      }
    }
  }
  return positions;
}

bool IsWideCodePoint(uint32_t cp) {
  const auto cls = ub_get_char_eaw_class(cp);
  return cls == EAW_W || cls == EAW_F;
}

bool IsOpenPunct(QChar ch) {
  static const QString kOpen = QString::fromUtf8("‘“（《〈「『【〖〔［｛");
  return kOpen.contains(ch);
}

bool IsClosePunct(QChar ch) {
  static const QString kClose = QString::fromUtf8("’”）》〉」』】〗〕］｝");
  return kClose.contains(ch);
}

bool IsStopPunct(QChar ch) {
  static const QString kStop = QString::fromUtf8("、：，；。．！？");
  return kStop.contains(ch);
}

bool IsNoBreakBefore(QChar ch) { return IsStopPunct(ch) || IsClosePunct(ch); }

bool IsCjkPunct(QChar ch) { return IsStopPunct(ch) || IsOpenPunct(ch) || IsClosePunct(ch); }

QStringList SplitParagraphs(const QString &text, bool preserve_structured_lines) {
  QString norm = text;
  norm.replace("\r\n", "\n");
  const QStringList lines = norm.split('\n');

  QStringList result;
  QString current;

  auto trim_para = [](const QString &p) -> QString {
    int st = 0;
    while (st < p.size() && (p[st] == ' ' || p[st] == '\t' || p[st] == QChar(0x3000) || p[st] == '\n')) {
      ++st;
    }
    int en = p.size();
    while (en > st && (p[en - 1] == ' ' || p[en - 1] == '\t' || p[en - 1] == QChar(0x3000) || p[en - 1] == '\n')) {
      --en;
    }
    return p.mid(st, en - st);
  };

  auto flush = [&]() {
    QString trimmed = trim_para(current);
    if (!trimmed.isEmpty()) {
      result.append(trimmed);
    }
    current.clear();
  };

  for (const QString &line : lines) {
    bool is_blank = true;
    for (QChar ch : line) {
      if (ch != ' ' && ch != '\t' && ch != QChar(0x3000)) {
        is_blank = false;
        break;
      }
    }
    if (is_blank) {
      flush();
      continue;
    }
    if (preserve_structured_lines) {
      const QString tline = line.trimmed();
      if (LooksLikeStructuredLine(tline) || LineHasIndentPrefix(line)) {
        flush();
        current = line;
        if (IsStandaloneHeadingLine(tline) || EndsWithParagraphTerminal(tline)) {
          flush();
        }
        continue;
      }
      if (!current.isEmpty()) {
        current.append('\n');
      }
      current.append(line);
      if (EndsWithParagraphTerminal(tline)) {
        flush();
      }
      continue;
    }
    if (!current.isEmpty()) {
      current.append('\n');
    }
    current.append(line);
  }
  flush();
  return result;
}

QString JoinLines(const QString &paragraph) {
  QString text = paragraph;
  text.replace("\r\n", "\n");
  int st = 0;
  while (st < text.size() && (text[st] == ' ' || text[st] == '\t' || text[st] == '\n')) {
    ++st;
  }
  int en = text.size();
  while (en > st && (text[en - 1] == ' ' || text[en - 1] == '\t' || text[en - 1] == '\n')) {
    --en;
  }
  text = text.mid(st, en - st);
  if (text.isEmpty()) {
    return {};
  }

  QString out;
  out.reserve(text.size());
  for (int i = 0; i < text.size();) {
    if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n') {
      const int run_start = i;
      bool has_nl = false;
      while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) {
        if (text[i] == '\n') {
          has_nl = true;
        }
        ++i;
      }
      if (!has_nl) {
        out.append(text.mid(run_start, i - run_start));
      } else if (!out.isEmpty() && i < text.size()) {
        const QChar before_ch = out.at(out.size() - 1);
        int dummy = 1;
        uint32_t before_cp = static_cast<uint32_t>(before_ch.unicode());
        if (out.size() >= 2 && before_ch.isLowSurrogate() && out.at(out.size() - 2).isHighSurrogate()) {
          before_cp = QChar::surrogateToUcs4(out.at(out.size() - 2), before_ch);
        }
        const uint32_t after_cp = CodePointAt(text, i, &dummy);
        if (!IsWideCodePoint(before_cp) && !IsWideCodePoint(after_cp)) {
          out.append(' ');
        }
      }
    } else {
      out.append(text[i]);
      ++i;
    }
  }
  return out;
}

std::vector<QString> Uax14LineBreakUnits(const QString &text) {
  std::vector<QString> units;
  if (text.isEmpty()) {
    return units;
  }
  EnsureLibunibreakInit();
  std::vector<char> brks(static_cast<size_t>(text.size()), 0);
  set_linebreaks_utf16(reinterpret_cast<const utf16_t *>(text.utf16()), static_cast<size_t>(text.size()), "",
                       brks.data());

  int start = 0;
  for (int i = 0; i < text.size(); ++i) {
    const char b = brks[i];
    if (b == LINEBREAK_MUSTBREAK || b == LINEBREAK_ALLOWBREAK || b == LINEBREAK_INDETERMINATE ||
        i + 1 == text.size()) {
      units.push_back(text.mid(start, i - start + 1));
      start = i + 1;
    }
  }
  return units;
}

std::vector<Piece> SplitUnit(const QString &unit, int base_offset) {
  int word_len = unit.size();
  while (word_len > 0 && unit[word_len - 1] == ' ') {
    --word_len;
  }
  if (word_len == 0) {
    return {};
  }

  const QString word = unit.left(word_len);
  const bool has_space = (word_len < unit.size());
  std::vector<Piece> pieces;

  // Pure Latin unit without "——" stays together (preserving curly quotes as Latin)
  if (IsAllWideFalse(word) && !word.contains(QString::fromUtf8("——"))) {
    pieces.push_back({word, true, has_space, true, base_offset, base_offset + word_len});
    return pieces;
  }

  for (int i = 0; i < word.size();) {
    int step = 1;
    const uint32_t cp = CodePointAt(word, i, &step);
    const QString ch_str = word.mid(i, step);
    const QChar ch = word[i];

    if (ch == QChar(0x2014) && !pieces.empty() && !pieces.back().is_latin &&
        pieces.back().text.endsWith(QChar(0x2014))) {
      pieces.back().text.append(ch_str);
      pieces.back().src_end = base_offset + i + step;
    } else if (IsCjkCharInUnit(cp, ch)) {
      if (!pieces.empty() && MayBreakAtQuote(pieces.back(), ch)) {
        pieces.back().allow_break_after = true;
      }
      pieces.push_back({ch_str, false, false, false, base_offset + i, base_offset + i + step});
    } else if (!pieces.empty() && pieces.back().is_latin) {
      pieces.back().text.append(ch_str);
      pieces.back().src_end = base_offset + i + step;
    } else {
      pieces.push_back({ch_str, true, false, false, base_offset + i, base_offset + i + step});
    }
    i += step;
  }

  pieces.back().has_trailing_space = has_space;
  pieces.back().allow_break_after = true;
  return pieces;
}

std::vector<Item> BuildParagraphItems(const QString &joined_text, const TypesetStyle &style,
                                      const ITextMetrics &metrics, bool enable_hyphenation) {
  const auto units = Uax14LineBreakUnits(joined_text);
  std::vector<Piece> pieces;
  int offset = 0;
  for (const QString &u : units) {
    auto split = SplitUnit(u, offset);
    pieces.insert(pieces.end(), split.begin(), split.end());
    offset += u.size();
  }
  if (pieces.empty()) {
    return {};
  }

  const double em = metrics.EmSize();
  const double space_w = metrics.MeasureAdvance(" ");
  const double autospace_w = style.autospace_em * em;
  const double hyphen_w = metrics.MeasureAdvance("-");

  const Item g_cjk = Item::Glue(0.0, style.cjk_stretch_em * em, style.cjk_shrink_em * em, QString());
  const Item g_punct = Item::Glue(0.5 * em, style.punct_stretch_em * em, style.punct_shrink_em * em, QString());
  const Item g_space = Item::Glue(space_w, space_w * 0.5, space_w / 3.0, " ");
  const Item g_autospace = Item::Glue(autospace_w, autospace_w * 0.5, autospace_w / 3.0, QString());
  const Item p_hyphen = Item::Penalty(kHyphenPenalty, hyphen_w, "-", true);
  const Item p_explicit_hyphen = Item::Penalty(kHyphenPenalty, 0.0, QString(), true);
  const Item p_no_break = Item::NoBreak();

  std::vector<Item> items;
  if (style.indent_em > 0.0) {
    items.push_back(Item::Box(style.indent_em * em, QString(qRound(style.indent_em), QChar(0x3000))));
  }

  const int runt_at = (style.runt_penalty > 0) ? FindRuntBreakIndex(pieces) : -1;

  for (size_t i = 0; i < pieces.size(); ++i) {
    const Piece &piece = pieces[i];
    const Piece *next_piece = (i + 1 < pieces.size()) ? &pieces[i + 1] : nullptr;

    if (!piece.is_latin) {
      if (piece.text.size() == 1 && (IsOpenPunct(piece.text[0]) || IsNoBreakBefore(piece.text[0]))) {
        const GlyphTrim trim = metrics.MeasurePunctTrim(piece.text, IsOpenPunct(piece.text[0]));
        items.push_back(Item::Box(trim.width, piece.text, trim.draw_dx, piece.src_start, piece.src_end));
      } else {
        items.push_back(
            Item::Box(metrics.MeasureAdvance(piece.text), piece.text, 0.0, piece.src_start, piece.src_end));
      }
    } else {
      const QString &word = piece.text;
      if (enable_hyphenation && style.hyphenator && style.hyphenator->IsLoaded()) {
        const auto pts = style.hyphenator->FindBreakPositions(word);
        int prev = 0;
        for (int pt : pts) {
          const QString seg = word.mid(prev, pt - prev);
          items.push_back(Item::Box(metrics.MeasureAdvance(seg), seg, 0.0, piece.src_start + prev,
                                    piece.src_start + pt));
          items.push_back(p_hyphen);
          prev = pt;
        }
        const QString tail = word.mid(prev);
        items.push_back(
            Item::Box(metrics.MeasureAdvance(tail), tail, 0.0, piece.src_start + prev, piece.src_end));
      } else {
        items.push_back(Item::Box(metrics.MeasureAdvance(word), word, 0.0, piece.src_start, piece.src_end));
      }
    }

    if (static_cast<int>(i) == runt_at) {
      items.push_back(Item::Penalty(style.runt_penalty));
    }

    if (!next_piece) {
      break;
    }

    if (!piece.allow_break_after) {
      items.push_back(p_no_break);
    }

    const QChar last = piece.text[piece.text.size() - 1];
    const QChar first = next_piece->text[0];

    if (piece.is_latin && next_piece->is_latin) {
      if (piece.has_trailing_space) {
        items.push_back(g_space);
      } else if (last == '-') {
        items.push_back(p_explicit_hyphen);
      } else {
        items.push_back(Item::ZeroGlue());
      }
    } else if (IsNoBreakBefore(last) || IsOpenPunct(first)) {
      items.push_back(piece.allow_break_after ? g_punct : g_cjk);
    } else if (piece.is_latin != next_piece->is_latin) {
      if (IsOpenPunct(last) || IsNoBreakBefore(first)) {
        items.push_back(g_cjk);
      } else {
        items.push_back(g_autospace);
      }
    } else if ((last == QChar(0x2026) && first == QChar(0x2026)) ||
               (last == QChar(0x2014) && first == QChar(0x2014))) {
      items.push_back(Item::ZeroGlue());
    } else {
      items.push_back(g_cjk);
    }
  }

  items.push_back(p_no_break);
  items.push_back(Item::InfGlue());
  items.push_back(Item::Eject());
  return items;
}

TypesetParagraph BreakParagraph3Pass(const QString &raw_paragraph, const TypesetStyle &style,
                                     const ITextMetrics &metrics, const std::vector<double> &line_widths) {
  TypesetParagraph result;
  result.joined_text = JoinLines(raw_paragraph);
  if (result.joined_text.isEmpty() || line_widths.empty()) {
    return result;
  }

  // Pass 1: without English hyphenation, rho = 1.0
  result.items = BuildParagraphItems(result.joined_text, style, metrics, false);
  if (result.items.empty()) {
    return result;
  }
  LineBreakOptions opts;
  opts.line_widths = line_widths;
  opts.max_stretch_ratio = 1.0;
  opts.looseness = style.looseness;
  opts.line_penalty = style.line_penalty;
  opts.kp81_penalty = style.kp81_penalty;
  opts.final_pass = false;

  if (auto brk = RunLineBreak(result.items, opts)) {
    result.lines = std::move(*brk);
    return result;
  }

  // Pass 2: with English hyphenation (if enabled), rho = 1.5
  const bool use_hyphen = (!style.hyphen_lang.isEmpty() && style.hyphenator && style.hyphenator->IsLoaded());
  if (use_hyphen) {
    result.items = BuildParagraphItems(result.joined_text, style, metrics, true);
  }
  opts.max_stretch_ratio = 1.5;
  if (auto brk = RunLineBreak(result.items, opts)) {
    result.lines = std::move(*brk);
    return result;
  }

  // Pass 3: final pass, rho = 2.0, guaranteed non-empty overfull fallback
  opts.max_stretch_ratio = 2.0;
  opts.final_pass = true;
  if (auto brk = RunLineBreak(result.items, opts)) {
    result.lines = std::move(*brk);
  }
  return result;
}

}  // namespace typesetting
}  // namespace gui
}  // namespace soeasy
