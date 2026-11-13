// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <iostream>
#include <vector>

#include "gui/document_viewer_widget.h"
#include "gui/icon_provider.h"
#include "gui/main_window.h"
#include "gui/preview_panel.h"
#include "gui/result_view.h"
#include "gui/search_bar.h"
#include "gui/search_table_model.h"

int main(int argc, char *argv[]) {
  // Support high-resolution crisp rendering
  QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

  QApplication app(argc, argv);

  soeasy::gui::MainWindow main_win(nullptr);
  main_win.resize(1190, 670);

  // 1. Set Mock Search Query
  auto *search_bar = main_win.findChild<soeasy::gui::SearchBar *>();
  if (search_bar) {
    search_bar->SetQuery("财务审计");
  }

  // 2. Set Splitter Ratio
  auto *splitter = main_win.findChild<QSplitter *>();
  if (splitter) {
    splitter->setSizes({680, 470});
  }

  // 3. Populate ResultView with 4 representative items
  auto *result_view = main_win.findChild<soeasy::gui::ResultView *>();
  if (result_view) {
    std::vector<soeasy::gui::DisplayResultItem> items;

    // Item 1: Excel Audit Report (Selected)
    {
      soeasy::gui::DisplayResultItem it;
      it.doc_id = 101;
      it.file_name = "2026年度第一季度财务审计报告.xlsx";
      it.file_path = "D:\\Workspaces\\Finance\\2026\\2026年度第一季度财务审计报告.xlsx";
      it.ext = "xlsx";
      it.file_size = 1258291;
      it.mtime = 1793444400;
      it.score = 14.82;
      items.push_back(std::move(it));
    }

    // Item 2: Word Spec
    {
      soeasy::gui::DisplayResultItem it;
      it.doc_id = 102;
      it.file_name = "企业级数据密态存储与合规设计说明.docx";
      it.file_path = "D:\\Workspaces\\Security\\Specs\\企业级数据密态存储与合规设计说明.docx";
      it.ext = "docx";
      it.file_size = 491520;
      it.mtime = 1793332500;
      it.score = 11.20;
      items.push_back(std::move(it));
    }

    // Item 3: PDF Invoices
    {
      soeasy::gui::DisplayResultItem it;
      it.doc_id = 103;
      it.file_name = "研发采购报销凭证与发票扫描件.pdf";
      it.file_path = "D:\\Workspaces\\Invoices\\2026\\研发采购报销凭证与发票扫描件.pdf";
      it.ext = "pdf";
      it.file_size = 3565158;
      it.mtime = 1793169720;
      it.score = 9.65;
      items.push_back(std::move(it));
    }

    // Item 4: Markdown Architecture
    {
      soeasy::gui::DisplayResultItem it;
      it.doc_id = 104;
      it.file_name = "核心服务架构设计与IPC通信协议.md";
      it.file_path = "D:\\Workspaces\\Architecture\\Docs\\核心服务架构设计与IPC通信协议.md";
      it.ext = "md";
      it.file_size = 18432;
      it.mtime = 1792891800;
      it.score = 8.40;
      items.push_back(std::move(it));
    }

    result_view->SetResults(items);

    // Explicitly configure clean column widths to eliminate any truncation
    using STCol = soeasy::gui::SearchTableModel;
    auto *hdr = result_view->horizontalHeader();
    hdr->setSectionResizeMode(STCol::kColIndex, QHeaderView::Fixed);
    result_view->setColumnWidth(STCol::kColIndex, 56);

    hdr->setSectionResizeMode(STCol::kColFileName, QHeaderView::Stretch);

    hdr->setSectionResizeMode(STCol::kColSize, QHeaderView::Fixed);
    result_view->setColumnWidth(STCol::kColSize, 85);

    hdr->setSectionResizeMode(STCol::kColMtime, QHeaderView::Fixed);
    result_view->setColumnWidth(STCol::kColMtime, 140);

    hdr->setSectionResizeMode(STCol::kColScore, QHeaderView::Fixed);
    result_view->setColumnWidth(STCol::kColScore, 65);

    result_view->selectRow(0);
  }

  // 4. Populate PreviewPanel
  auto *preview_panel = main_win.findChild<soeasy::gui::PreviewPanel *>();
  if (preview_panel) {
    // Specifically target the header labels inside header card
    auto labels = preview_panel->findChildren<QLabel *>();
    for (auto *lbl : labels) {
      if (lbl->text().contains("未选中文件")) {
        lbl->setText("2026年度第一季度财务审计报告.xlsx");
      } else if (lbl->text().contains("无正文提取内容")) {
        lbl->setText("D:\\Workspaces\\Finance\\2026\\2026年度第一季度财务审计报告.xlsx");
      }
    }

    // Set genuine Excel file icon
    for (auto *lbl : labels) {
      if (lbl->pixmap() == nullptr && lbl->styleSheet().contains("border: none;") && lbl->text().isEmpty()) {
        lbl->setPixmap(soeasy::gui::IconProvider::GetFileIcon("xlsx").pixmap(24, 24));
      }
    }

    // Action buttons
    auto btns = preview_panel->findChildren<QPushButton *>();
    for (auto *btn : btns) {
      if (btn->text().contains("打开文件") || btn->text().contains("所在目录")) {
        btn->setEnabled(true);
      }
    }

    // Load rich text with highlighted keywords
    QString mock_text =
        "【2026年度第一季度财务审计报告与研发预算执行汇总】\n\n"
        "一、审计背景与执行准则\n"
        "本季度财务审计工作严格依据国家企业财务通则与内控合规审计规范展开。\n"
        "重点审查高性能算法集群算力采购、底层基础设施工程建设及各项合规支出。\n\n"
        "二、关键研发收支明细清单\n"
        "1. 高性能算法与异构算力集群专项采购：$ 380,000.00\n"
        "2. 资深基础架构与数据安全审计专项支出：$ 450,000.00\n"
        "3. 全季度共计 128 项研发报销凭据已全数完成合规财务审计核销。\n"
        "4. 本地密态存储系统与透明加密基础设施全面交付上线。\n\n"
        "三、独立审计结论意见\n"
        "经全面财务审计与数据交叉复核，本季度财务报告客观、公允地反映了各项资产配置、\n"
        "工程研发投入及核心知识产权投资现况。内控机制健全，各项合规指标运行优良。\n\n"
        "--------------------------------------------------\n"
        "审计单位：SoEasy 联合财务审计复核组\n"
        "审计完成日期：2026年10月31日";

    if (preview_panel->viewer()) {
      preview_panel->viewer()->LoadContent(mock_text, {"财务审计", "财务报告"});
    }
  }

  // 5. Update status bar labels
  auto all_labels = main_win.findChildren<QLabel *>();
  for (auto *lbl : all_labels) {
    if (lbl->text().contains("在索引文件") || lbl->text().contains("待扫描")) {
      lbl->setText("在索引文件: 18,492 个 | 密态存储: 142.6 MB (Zstd密态压缩)");
    } else if (lbl->text().contains("检索耗时") || lbl->text().contains("就绪")) {
      lbl->setText("● 检索耗时: 0.38 ms (4 个结果)");
    }
  }

  main_win.show();
  app.processEvents();

  // Grab pristine window rendering
  QPixmap pix = main_win.grab();
  QString out_path = "/tmp/soeasy_real_window_raw.png";
  if (argc > 1) {
    out_path = argv[1];
  }
  pix.save(out_path);
  std::cout << "SUCCESS: Real window rendered to " << out_path.toStdString() << "\n";

  return 0;
}
