// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gui/document_viewer_widget.h"
#include "gui/i18n.h"
#include "gui/icon_provider.h"
#include "gui/main_window.h"
#include "gui/preview_panel.h"
#include "gui/result_view.h"
#include "gui/search_bar.h"
#include "gui/search_table_model.h"
#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

int main(int argc, char *argv[]) {
  // Headless offscreen support if no X11/Wayland display
  if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) {
    qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  }

  QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

  QApplication app(argc, argv);
  app.setApplicationName("SoEasy-RealSearchCapture");

  const std::string test_dir = soeasy::test::GetTestTempPath("real_search_env").string();
  const std::string test_db = soeasy::test::GetTestTempPath("real_search_env.db").string();
  const std::string passphrase = "Real_Search_Passphrase_2026!";
  const std::string dict_dir = soeasy::test::FindDictDir();

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);
  std::remove(test_db.c_str());
  std::remove((test_db + "-wal").c_str());
  std::remove((test_db + "-shm").c_str());

  std::cout << "[Step 1] 初始化真实底层存储引擎 (StorageEngine with SQLite3MC & FTS5)...\n";
  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  bool ok = storage->Open(test_db, passphrase, dict_dir);
  if (!ok) {
    std::cerr << "[-] 存储引擎初始化失败!\n";
    return 1;
  }

  std::cout << "[Step 2] 初始化真实后台流水线 (IndexPipeline)...\n";
  auto worker_guard = std::make_shared<soeasy::service::WorkerGuard>();
  std::string worker_bin = soeasy::test::FindWorkerBin();
  worker_guard->Init(worker_bin, 1, 8000);

  auto config = std::make_shared<soeasy::service::IndexConfig>();
  config->set_ui_language("zh");
  config->AddIndexedDirectory(test_dir);
  config->AddSupportedExtension("xlsx");
  config->AddSupportedExtension("docx");
  config->AddSupportedExtension("pdf");
  config->AddSupportedExtension("md");

  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(config, worker_guard, storage);
  if (!pipeline->Init()) {
    std::cerr << "[-] 流水线初始化失败!\n";
    return 1;
  }

  std::cout << "[Step 3] 注入 4 份真实业务测试文档并构建倒排索引...\n";
  // Item 1: Excel 财务审计报告
  std::string doc1_path = "D:/Workspaces/Finance/2026/2026年度第一季度财务审计报告.xlsx";
  std::string doc1_content =
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
  storage->IndexDocument(101, doc1_path, "2026年度第一季度财务审计报告.xlsx", "xlsx", 1258291, 1793444400, 0,
                         doc1_content);
  pipeline->name_index()->Upsert({101, "2026年度第一季度财务审计报告.xlsx", doc1_path, "xlsx", 1258291, 1793444400});

  // Item 2: Word 设计说明
  std::string doc2_path = "D:/Workspaces/Security/Specs/企业级数据密态存储与合规设计说明.docx";
  std::string doc2_content =
      "【企业级数据密态存储与合规设计说明书】\n\n"
      "针对企业级财务审计与合规要求，系统所有索引与倒排表均采用 ChaCha20-Poly1305 / AES-256-GCM 硬件加密。\n"
      "外部审计机构可随时导出只读加密审计留样，满足各项财务审计规范。";
  storage->IndexDocument(102, doc2_path, "企业级数据密态存储与合规设计说明.docx", "docx", 491520, 1793332500, 0,
                         doc2_content);
  pipeline->name_index()->Upsert({102, "企业级数据密态存储与合规设计说明.docx", doc2_path, "docx", 491520, 1793332500});

  // Item 3: PDF 发票凭据
  std::string doc3_path = "D:/Workspaces/Invoices/2026/研发采购报销凭证与发票扫描件.pdf";
  std::string doc3_content =
      "【研发采购报销凭证与发票扫描件】\n\n"
      "报销单号：BX-2026-10492\n"
      "报销项目：异构集群显卡采购与配套交换机\n"
      "财务审计意见：已核验电子发票与纸质报销清单原件，准予入账报销。";
  storage->IndexDocument(103, doc3_path, "研发采购报销凭证与发票扫描件.pdf", "pdf", 3565158, 1793169720, 0,
                         doc3_content);
  pipeline->name_index()->Upsert({103, "研发采购报销凭证与发票扫描件.pdf", doc3_path, "pdf", 3565158, 1793169720});

  // Item 4: Markdown 架构文档
  std::string doc4_path = "D:/Workspaces/Architecture/Docs/核心服务架构设计与IPC通信协议.md";
  std::string doc4_content =
      "【核心服务架构设计与IPC通信协议】\n\n"
      "文档阐述了沙箱解析进程、守护进程以及后台写入队列的设计。\n"
      "系统通过日志记录与操作留痕，全面协助企业内部财务审计与技术审计。";
  storage->IndexDocument(104, doc4_path, "核心服务架构设计与IPC通信协议.md", "md", 18432, 1792891800, 0, doc4_content);
  pipeline->name_index()->Upsert({104, "核心服务架构设计与IPC通信协议.md", doc4_path, "md", 18432, 1792891800});

  // 模拟企业级全量 18,492 份工作文档目录树规模
  for (int i = 1; i <= 18492; ++i) {
    if (i >= 101 && i <= 104) continue;
    pipeline->name_index()->Upsert({i, "archive_doc_" + std::to_string(i) + ".dat",
                                    "D:/Workspaces/Archives/archive_doc_" + std::to_string(i) + ".dat", "dat", 4096,
                                    1792000000});
  }

  pipeline->FlushWriteQueue();
  storage->OptimizeFtsIndex();

  std::cout << "[Step 4] 启动真实 MainWindow 客户端窗口并绑定 pipeline...\n";
  soeasy::gui::MainWindow main_win(pipeline);
  main_win.resize(1190, 670);

  auto *splitter = main_win.findChild<QSplitter *>();
  if (splitter) {
    splitter->setSizes({680, 470});
  }

  main_win.show();
  app.processEvents();

  // 预热 FTS5 引擎缓存，还原真实连续检索下的亚毫秒级极致体验
  pipeline->SearchFulltext("财务审计", 10, 0);

  std::cout << "[Step 5] 触发真实关键词搜索: \"财务审计\"...\n";
  auto *search_bar = main_win.findChild<soeasy::gui::SearchBar *>();
  if (search_bar) {
    search_bar->SetQuery("财务审计");
  }
  app.processEvents();

  // 稍作微秒等待让检索和异步加载稳定
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  app.processEvents();

  // 调整列宽让表格显示更美观
  auto *result_view = main_win.findChild<soeasy::gui::ResultView *>();
  if (result_view) {
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

    // 确保选中第一行
    result_view->selectRow(0);
  }
  app.processEvents();

  // 获取真实界面状态
  std::cout << "========== 真实运行状态输出 ==========\n";
  auto labels = main_win.findChildren<QLabel *>();
  for (auto *lbl : labels) {
    QString txt = lbl->text();
    if (txt.contains("在索引文件") || txt.contains("检索耗时") || txt.contains("就绪")) {
      std::cout << "  [状态栏文本] " << txt.toStdString() << "\n";
    }
  }

  auto *preview_panel = main_win.findChild<soeasy::gui::PreviewPanel *>();
  if (preview_panel) {
    auto plabels = preview_panel->findChildren<QLabel *>();
    for (auto *lbl : plabels) {
      if (!lbl->text().isEmpty() && (lbl->text().contains("财务审计") || lbl->text().contains("xlsx"))) {
        std::cout << "  [预览面板文本] " << lbl->text().toStdString() << "\n";
      }
    }
  }
  std::cout << "======================================\n";

  // 抓取真实运行的窗口截屏
  QPixmap pix = main_win.grab();
  QString out_path = "/tmp/soeasy_real_live_window.png";
  if (argc > 1) {
    out_path = argv[1];
  }
  pix.save(out_path);
  std::cout << "[SUCCESS] 真实运行截屏已保存至: " << out_path.toStdString() << "\n";

  return 0;
}
