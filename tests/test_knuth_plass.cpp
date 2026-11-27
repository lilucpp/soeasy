// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <QApplication>
#include <QFileInfo>
#include <cmath>
#include <iostream>
#include <vector>

#include "gui/typesetting/cjk_typesetter.h"
#include "gui/typesetting/kp_engine.h"
#include "gui/typesetting/kp_types.h"
#include "gui/typesetting/preview_typography.h"

namespace {

using namespace soeasy::gui::typesetting;

int g_failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n";              \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (false)

class FixedTestMetrics : public ITextMetrics {
 public:
  explicit FixedTestMetrics(double em = 10.0) : em_(em) {}

  double EmSize() const override { return em_; }

  double MeasureAdvance(const QString &text) const override {
    double w = 0.0;
    for (QChar ch : text) {
      if (ch == ' ') {
        w += em_ * 0.35;
      } else if (ch.unicode() >= 0x2E80) {
        w += em_;
      } else {
        w += em_ * 0.55;
      }
    }
    return w;
  }

  GlyphTrim MeasurePunctTrim(const QString &ch, bool is_open_punct) const override {
    if (ch.isEmpty()) return {};
    if (IsOpenPunct(ch.at(0))) {
      return {em_ * 0.55, is_open_punct ? -em_ * 0.45 : 0.0};
    }
    if (IsClosePunct(ch.at(0)) || IsStopPunct(ch.at(0))) {
      return {em_ * 0.55, 0.0};
    }
    return {em_, 0.0};
  }

 private:
  double em_;
};

std::vector<Item> MakeSimpleWords(const std::vector<double> &widths, double space_w = 5.0,
                                  double stretch = 3.0, double shrink = 2.0) {
  std::vector<Item> items;
  for (size_t i = 0; i < widths.size(); ++i) {
    if (i > 0) {
      items.push_back(Item::Glue(space_w, stretch, shrink, " "));
    }
    items.push_back(Item::Box(widths[i], QString("w%1").arg(i)));
  }
  items.push_back(Item::InfGlue());
  items.push_back(Item::Eject());
  return items;
}

void TestBasicKnuthPlass() {
  std::cout << "[1/6] Testing RunLineBreak single-width, infeasible, and final_pass...\n";
  // Four 35pt words with 5pt space -> pairs fit exactly into 75pt lines (ratio == 0).
  const auto exact_items = MakeSimpleWords({35.0, 35.0, 35.0, 35.0});
  LineBreakOptions opts;
  opts.line_widths = {75.0};
  opts.max_stretch_ratio = 1.0;

  auto res = RunLineBreak(exact_items, opts);
  CHECK(res.has_value());
  if (res.has_value()) {
    CHECK(res->size() == 2);
    CHECK((*res)[0].line == 1);
    CHECK((*res)[0].start == 0);
    CHECK((*res)[1].line == 2);
  }

  // Insufficient stretch (needed ratio 3.33 > max_stretch_ratio 1.0) returns nullopt.
  const auto tight_items = MakeSimpleWords({30.0, 30.0, 30.0, 30.0});
  CHECK(!RunLineBreak(tight_items, opts).has_value());

  // Huge unbreakable box exceeds width cleanly when final_pass == false, succeeds on final_pass == true.
  const auto huge = MakeSimpleWords({30.0, 120.0, 30.0});
  LineBreakOptions strict_opts;
  strict_opts.line_widths = {75.0};
  strict_opts.max_stretch_ratio = 1.0;
  strict_opts.final_pass = false;
  CHECK(!RunLineBreak(huge, strict_opts).has_value());

  LineBreakOptions fallback_opts = strict_opts;
  fallback_opts.final_pass = true;
  auto fallback = RunLineBreak(huge, fallback_opts);
  CHECK(fallback.has_value());
  if (fallback.has_value()) {
    // tex.web §854 sets [w0 w1] on overfull line 1, and [w2] on line 2.
    CHECK(fallback->size() == 2);
  }
}

void TestVaryingWidthsAndLooseness() {
  std::cout << "[2/6] Testing varying line_widths, looseness, and forced ejects...\n";
  // Classic varying line widths (like dropcap/parshape: first line 45pt, rest 80pt)
  const auto items = MakeSimpleWords({20.0, 20.0, 20.0, 20.0, 20.0, 20.0}, 5.0, 4.0, 2.0);
  LineBreakOptions shape_opts;
  shape_opts.line_widths = {45.0, 80.0};
  shape_opts.max_stretch_ratio = 2.0;

  auto shape_res = RunLineBreak(items, shape_opts);
  CHECK(shape_res.has_value());
  if (shape_res.has_value()) {
    // First line width 45pt fits w0 (0), Glue (1), w1 (2), breaking at Glue index 3.
    CHECK((*shape_res)[0].start == 0);
    CHECK((*shape_res)[0].end <= 3);
  }

  // Forced break inside paragraph resets running totals without underflow/drift.
  std::vector<Item> forced;
  forced.push_back(Item::Box(25.0, "a"));
  forced.push_back(Item::Glue(5.0, 3.0, 1.0));
  forced.push_back(Item::Box(25.0, "b"));
  forced.push_back(Item::Eject());
  forced.push_back(Item::Box(30.0, "c"));
  forced.push_back(Item::InfGlue());
  forced.push_back(Item::Eject());

  LineBreakOptions f_opts;
  f_opts.line_widths = {80.0};
  f_opts.max_stretch_ratio = 2.0;
  f_opts.final_pass = true;
  auto f_res = RunLineBreak(forced, f_opts);
  CHECK(f_res.has_value());
  if (f_res.has_value()) {
    CHECK(f_res->size() == 2);
    CHECK((*f_res)[1].start == 4);
  }

  // Looseness +1 vs 0 on elastic text
  const auto elastic = MakeSimpleWords({25.0, 25.0, 25.0, 25.0, 25.0, 25.0}, 6.0, 8.0, 2.0);
  LineBreakOptions tight_opts;
  tight_opts.line_widths = {85.0};
  tight_opts.max_stretch_ratio = 3.0;
  tight_opts.looseness = 0;
  auto r0 = RunLineBreak(elastic, tight_opts);

  LineBreakOptions loose_opts = tight_opts;
  loose_opts.looseness = 1;
  auto r_plus = RunLineBreak(elastic, loose_opts);
  CHECK(r0.has_value() && r_plus.has_value());
  if (r0.has_value() && r_plus.has_value()) {
    CHECK(r_plus->size() >= r0->size());
  }
}

void TestParagraphSplitAndJoinLines() {
  std::cout << "[3/6] Testing SplitParagraphs and JoinLines...\n";
  const QString prose =
      "First line of\nparagraph one.\n\n"
      "- Item alpha\n- Item beta\n\n"
      "中文第一行\n中文第二行与 English\nhere.";

  const QStringList paras = SplitParagraphs(prose, true);
  CHECK(paras.size() == 4);
  CHECK(paras[0].contains("paragraph one."));
  CHECK(paras[1] == "- Item alpha");
  CHECK(paras[2] == "- Item beta");

  const QStringList plain_paras = SplitParagraphs(prose, false);
  CHECK(plain_paras.size() == 3);

  CHECK(JoinLines("中文第一行\n中文第二行") == QStringLiteral("中文第一行中文第二行"));
  CHECK(JoinLines("hello\nworld") == QStringLiteral("hello world"));
  CHECK(JoinLines("中文\nEnglish") == QStringLiteral("中文English"));

  const QString headed = "# Section Title\nParagraph body line.";
  const QStringList headed_paras = SplitParagraphs(headed, true);
  CHECK(headed_paras.size() == 2);

  // Single-newline real-world regulatory / DOCX extracted text must NOT merge into 1 giant block
  const QString regulation =
      "普通话水平测试管理规定\n"
      "（2021年11月27日中华人民共和国教育部令第51号公布自2022年1月1日起施行）\n"
      "第一条为规范普通话水平测试管理，制定本规定。\n"
      "第三条国务院语言文字工作部门主管全国的测试工作。\n"
      "省、自治区、直辖市人民政府语言文字工作部门主管本行政区域内的测试工作。\n"
      "第十条下列人员应当接受测试：\n"
      "（一）教师；\n"
      "（二）广播电台、电视台的播音员、节目主持人；";
  const QStringList reg_paras = SplitParagraphs(regulation, true);
  CHECK(reg_paras.size() == 8);
  CHECK(reg_paras[0] == QStringLiteral("普通话水平测试管理规定"));
  CHECK(reg_paras[2].startsWith(QStringLiteral("第一条")));
  CHECK(reg_paras[4].startsWith(QStringLiteral("省、自治区")));
  CHECK(reg_paras[6] == QStringLiteral("（一）教师；"));
}

void TestUax14AndMayBreakAtQuote() {
  std::cout << "[4/6] Testing libunibreak UAX #14 Kinsoku and quote break units...\n";
  const QString sample = QStringLiteral("中文“引用”，测试结束。");
  const auto units = Uax14LineBreakUnits(sample);
  CHECK(!units.empty());

  // Leading open quote must bind to following char, closing punct must bind to preceding char.
  bool found_close_with_comma = false;
  bool isolated_period = false;
  for (const QString &u : units) {
    if (u.contains(QStringLiteral("引用")) || u.contains(QStringLiteral("”，"))) {
      found_close_with_comma = true;
    }
    if (u == QStringLiteral("。")) {
      isolated_period = true;
    }
  }
  CHECK(!isolated_period);
  CHECK(found_close_with_comma);
}

void TestCjkLatinAutospaceAndSpans() {
  std::cout << "[5/6] Testing BuildParagraphItems CJK/Latin autospace & UTF-16 source spans...\n";
  FixedTestMetrics metrics(10.0);
  TypesetStyle style;
  style.size = 10.0;
  style.autospace_em = 0.25;

  const QString text = QStringLiteral("采用Knuth算法进行排版。");
  const std::vector<Item> items = BuildParagraphItems(text, style, metrics, false);
  CHECK(!items.empty());

  // Verify CJK/Latin boundary inserts Glue(2.5, ...) without altering source span continuity.
  int autospace_glue_count = 0;
  QString reconstructed;
  for (const Item &it : items) {
    if (it.kind == ItemKind::kBox && it.src_start >= 0 && it.src_end > it.src_start) {
      CHECK(it.src_end <= text.size());
      CHECK(text.mid(it.src_start, it.src_end - it.src_start) == it.text);
      reconstructed += it.text;
    } else if (it.kind == ItemKind::kGlue && it.text.isEmpty() && std::abs(it.width - 2.5) < 1e-6) {
      ++autospace_glue_count;
    }
  }
  CHECK(reconstructed == text);
  CHECK(autospace_glue_count == 2);
}

void TestHyphenationPass2() {
  std::cout << "[6/6] Testing bundled hyph_en_US.dic loading and 3-pass hyphenation fallback...\n";
  const QString dic_path = PreviewTypographyStore::FindHyphenDictFile("en_US");
  CHECK(!dic_path.isEmpty());
  CHECK(QFileInfo::exists(dic_path));

  Hyphenator hyph;
  CHECK(hyph.LoadFromFile(dic_path));
  CHECK(hyph.IsLoaded());

  const auto breaks = hyph.FindBreakPositions("internationalization");
  CHECK(!breaks.empty());
  for (int pos : breaks) {
    // Verify \lefthyphenmin=2 \righthyphenmin=3 bounds
    CHECK(pos >= 2);
    CHECK(20 - pos >= 3);
  }

  FixedTestMetrics metrics(12.0);
  TypesetStyle style;
  style.size = 12.0;
  style.hyphen_lang = "en_US";
  style.hyphenator = &hyph;

  const QString para = QStringLiteral("Ultra internationalization considerations require thoughtful typography.");
  // Narrow width triggers Pass-2 hyphenation where unhyphenated words would overflow.
  const TypesetParagraph broken = BreakParagraph3Pass(para, style, metrics, {95.0});
  CHECK(!broken.lines.empty());
  bool has_hyphen_penalty_item = false;
  for (const Item &it : broken.items) {
    if (it.kind == ItemKind::kPenalty && it.is_hyphen) {
      has_hyphen_penalty_item = true;
      break;
    }
  }
  CHECK(has_hyphen_penalty_item);
}

}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);

  TestBasicKnuthPlass();
  TestVaryingWidthsAndLooseness();
  TestParagraphSplitAndJoinLines();
  TestUax14AndMayBreakAtQuote();
  TestCjkLatinAutospaceAndSpans();
  TestHyphenationPass2();

  if (g_failures > 0) {
    std::cerr << "test_knuth_plass FAILED with " << g_failures << " error(s).\n";
    return 1;
  }
  std::cout << "All test_knuth_plass checks PASSED!\n";
  return 0;
}
