// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardPaths>
#include <cmath>
#include <iostream>

#include "gui/document_viewer_widget.h"
#include "gui/knuth_plass_viewer.h"
#include "gui/typesetting/preview_typography.h"
#include "gui/typography_popover.h"

namespace {

using namespace soeasy::gui;
using namespace soeasy::gui::typesetting;

int g_failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << #cond << "\n";              \
      ++g_failures;                                                                                \
    }                                                                                              \
  } while (false)

void TestBundledFontsLoad() {
  std::cout << "[1/5] Testing bundled WenYuanSansSC & LXGWWenKaiLite loading...\n";
  const QString wenyuan_path = PreviewTypographyStore::FindFontFile("WenYuanSansSC-Regular.otf");
  const QString lxgw_path = PreviewTypographyStore::FindFontFile("LXGWWenKaiLite-Regular.ttf");
  CHECK(!wenyuan_path.isEmpty() && QFileInfo::exists(wenyuan_path));
  CHECK(!lxgw_path.isEmpty() && QFileInfo::exists(lxgw_path));

  auto &store = PreviewTypographyStore::Instance();
  store.ResetDefaults();
  const auto &defs = store.config();
  CHECK(defs.font_choice == "lxgw-wenkai");
  CHECK(defs.font_size_px == 20);
  CHECK(!defs.auto_width);
  CHECK(std::abs(defs.text_width_em - 22.0) < 1e-6);
  CHECK(std::abs(defs.indent_em - 2.0) < 1e-6);
  CHECK(std::abs(defs.par_skip_em - 0.5) < 1e-6);
  CHECK(std::abs(defs.line_skip - 1.4) < 1e-6);
  CHECK(defs.hyphen_lang == "en_US");
  CHECK(defs.runt_penalty == 0);
  CHECK(defs.looseness == 0);
  CHECK(defs.theme_preset == "sepia");
  CHECK(defs.show_guides);

  TypographyPopover popover;
  const auto int_spins = popover.findChildren<QSpinBox *>();
  const auto dbl_spins = popover.findChildren<QDoubleSpinBox *>();
  CHECK(int_spins.size() == 2);  // font_size, runt_penalty (no looseness)
  CHECK(dbl_spins.size() == 3);  // text_width, indent, par_skip (no line_skip)
  for (auto *sp : int_spins) CHECK(sp->suffix().isEmpty());
  for (auto *sp : dbl_spins) CHECK(sp->suffix().isEmpty());

  PreviewTypographyConfig cfg;
  cfg.font_choice = "wenyuan-sans";
  cfg.font_size_px = 14;
  const QFont f_wy = store.ResolveFont(cfg);
  CHECK(!f_wy.family().isEmpty());

  cfg.font_choice = "lxgw-wenkai";
  const QFont f_lx = store.ResolveFont(cfg);
  CHECK(!f_lx.family().isEmpty());
  CHECK(f_wy.family() != f_lx.family());
}

void TestCodeVsProseRoutingAndToggle() {
  std::cout << "[2/5] Testing code vs prose viewer routing and KP cancel/re-enable...\n";
  auto &store = PreviewTypographyStore::Instance();
  store.ResetDefaults();

  DocumentViewerWidget viewer;
  viewer.resize(520, 640);
  viewer.show();
  QApplication::processEvents();

  // Prose files -> KP active by default
  const QString sample_prose =
      "SoEasy 全文检索引擎支持 Knuth-Plass 段落断行精排。\n\n"
      "第二段包含 English typography 与中文标点：“引号、顿号、句号。”";
  viewer.LoadContent(sample_prose, {"Knuth-Plass", "精排"}, "/tmp/report.md");
  QApplication::processEvents();
  CHECK(!viewer.is_code_document());
  CHECK(viewer.is_kp_active());
  CHECK(viewer.match_count() == 2);

  // Clicking KP toggle turns KP off -> switches to QTextBrowser AND preserves matches
  auto buttons = viewer.findChildren<QPushButton *>();
  QPushButton *kp_btn = nullptr;
  for (auto *btn : buttons) {
    if (btn->isCheckable()) {
      kp_btn = btn;
      break;
    }
  }
  CHECK(kp_btn != nullptr && kp_btn->isChecked());
  if (kp_btn) {
    kp_btn->click();
    QApplication::processEvents();
    CHECK(!viewer.is_kp_active());
    CHECK(!store.config().kp_enabled);
    CHECK(viewer.text_viewer()->toPlainText().contains("Knuth-Plass"));
    CHECK(viewer.match_count() == 2);

    // Re-enable KP
    kp_btn->click();
    QApplication::processEvents();
    CHECK(viewer.is_kp_active());
    CHECK(store.config().kp_enabled);
  }

  // Code / config / CSV / log files -> monospace QTextBrowser by default WITHOUT disabling global KP
  const std::vector<QString> code_paths = {"/tmp/main.cpp", "/tmp/script.py", "/tmp/config.json",
                                           "/tmp/data.csv", "/tmp/server.log"};
  for (const QString &p : code_paths) {
    viewer.LoadContent("int value = 42;\nreturn value;", {"value"}, p);
    QApplication::processEvents();
    CHECK(viewer.is_code_document());
    CHECK(!viewer.is_kp_active());
    CHECK(store.config().kp_enabled);
    CHECK(viewer.match_count() == 2);
  }

  // Loading prose again restores KP automatically
  viewer.LoadContent(sample_prose, {"全文"}, "/tmp/notes.txt");
  QApplication::processEvents();
  CHECK(!viewer.is_code_document());
  CHECK(viewer.is_kp_active());

  store.ResetDefaults();
}

void TestMatchNavigationAndPaint() {
  std::cout << "[3/5] Testing KnuthPlassViewer match navigation, selection, and QPainter render...\n";
  KnuthPlassViewer kp;
  kp.resize(480, 360);
  kp.show();

  const QString doc =
      "First occurrence of search keyword alpha.\n\n"
      "Second paragraph mentioning alpha again in Mixed 中文内容。\n\n"
      "Third paragraph closing with Alpha.";
  kp.SetContent(doc, {"alpha"});
  QApplication::processEvents();

  CHECK(kp.MatchCount() == 3);
  CHECK(kp.CurrentMatchIndex() == 0);
  kp.JumpToMatch(1);
  CHECK(kp.CurrentMatchIndex() == 1);
  kp.JumpToMatch(2);
  CHECK(kp.CurrentMatchIndex() == 2);

  kp.SelectAllText();
  const QString selected = kp.SelectedText();
  CHECK(selected.contains("First occurrence"));
  CHECK(selected.contains("Third paragraph"));

  // Render onto an offscreen image to verify paintEvent path executes cleanly.
  QImage surface(kp.size(), QImage::Format_ARGB32_Premultiplied);
  kp.render(&surface);
  CHECK(!surface.isNull());
}

void TestHtmlNoticeResetsKpMode() {
  std::cout << "[4/5] Testing SetHtml clears KP view and switches back to QTextBrowser...\n";
  DocumentViewerWidget viewer;
  viewer.resize(460, 400);
  viewer.LoadContent("Prose paragraph for KP mode.", {}, "/tmp/readme.txt");
  CHECK(viewer.is_kp_active());

  viewer.SetHtml("<p>Document is encrypted.</p>");
  CHECK(!viewer.is_kp_active());
  CHECK(viewer.text_viewer()->toPlainText().contains("encrypted"));
}

void TestLargeDocumentLazyLayoutPerf() {
  std::cout << "[5/6] Testing ~1MB prose lazy layout & late-paragraph JumpToMatch...\n";
  QString para =
      "在现代桌面全量检索引擎的工程实践中，Knuth-Plass 全局最优动态规划断行算法能够显著改善中英文混排段落的版面灰度均衡性。\n\n";
  QString large_doc;
  large_doc.reserve(1050000);
  while (large_doc.toUtf8().size() < 950000) {
    large_doc += para;
  }
  large_doc += "TailTargetMarker 位于长文档末尾段落用于验证无级联跳转效率。";

  KnuthPlassViewer kp;
  kp.resize(500, 640);
  kp.show();

  QElapsedTimer timer;
  timer.start();
  kp.SetContent(large_doc, {"TailTargetMarker"});
  QApplication::processEvents();
  const qint64 elapsed_ms = timer.elapsed();

  std::cout << "  -> Loaded " << kp.ParagraphCount() << " paragraphs in " << elapsed_ms
            << " ms (laid out initially: " << kp.LaidOutParagraphCount() << ")\n";
  CHECK(kp.ParagraphCount() > 1000);
  CHECK(kp.LaidOutParagraphCount() < 120);
  CHECK(kp.LaidOutParagraphCount() < kp.ParagraphCount() / 4);
  CHECK(elapsed_ms < 2000);

  QElapsedTimer jump_timer;
  jump_timer.start();
  kp.JumpToMatch(0);
  QApplication::processEvents();
  const qint64 jump_ms = jump_timer.elapsed();
  std::cout << "  -> JumpToMatch to tail paragraph (" << kp.ParagraphCount() - 1 << ") took " << jump_ms
            << " ms, scrollY=" << kp.verticalScrollBar()->value() << "\n";
  CHECK(kp.verticalScrollBar()->value() > 0);
  CHECK(kp.LaidOutParagraphCount() == kp.ParagraphCount());
  CHECK(jump_ms < 5000);
}

void TestSurrogatePairAtomicSelection() {
  std::cout << "[6/6] Testing non-BMP UTF-16 surrogate pair atomic hit-testing & selection...\n";
  KnuthPlassViewer kp;
  kp.resize(520, 300);
  kp.show();

  // U+20BB7 (𠮷) and U+1F600 (😀) are 2-unit UTF-16 surrogate pairs
  const QString sample = QString::fromUtf8("前文\xF0\xA0\xAE\xB7字与\xF0\x9F\x98\x80符号。");
  kp.SetContent(sample, {std::string("\xF0\xA0\xAE\xB7")});
  QApplication::processEvents();

  CHECK(kp.MatchCount() == 1);

  // Sweep mouse drag positions across line 1 and verify SelectedText() never splits a surrogate pair.
  for (int x = 35; x < 260; x += 5) {
    QMouseEvent press(QEvent::MouseButtonPress, QPoint(35, 28), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, QPoint(x, 28), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, QPoint(x, 28), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(kp.viewport(), &press);
    QApplication::sendEvent(kp.viewport(), &move);
    QApplication::sendEvent(kp.viewport(), &release);
    const QString sel = kp.SelectedText();
    for (int i = 0; i < sel.size(); ++i) {
      if (sel.at(i).isHighSurrogate()) {
        CHECK(i + 1 < sel.size() && sel.at(i + 1).isLowSurrogate());
        ++i;
      } else {
        CHECK(!sel.at(i).isLowSurrogate());
      }
    }
  }
}

}  // namespace

int main(int argc, char **argv) {
  QStandardPaths::setTestModeEnabled(true);
  QApplication app(argc, argv);

  TestBundledFontsLoad();
  TestCodeVsProseRoutingAndToggle();
  TestMatchNavigationAndPaint();
  TestHtmlNoticeResetsKpMode();
  TestLargeDocumentLazyLayoutPerf();
  TestSurrogatePairAtomicSelection();

  if (g_failures > 0) {
    std::cerr << "test_knuth_plass_viewer FAILED with " << g_failures << " error(s).\n";
    return 1;
  }
  std::cout << "All test_knuth_plass_viewer checks PASSED!\n";
  return 0;
}
