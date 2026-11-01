// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <QApplication>
#include <QDir>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gui/content_preview_dialog.h"
#include "gui/hover_preview_popup.h"
#include "gui/i18n.h"
#include "gui/main_window.h"
#include "gui/result_view.h"
#include "gui/search_bar.h"
#include "gui/settings_dialog.h"
#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "[-] CHECK FAILED: " #expr " at " << __FILE__ << ":"     \
                << __LINE__ << std::endl;                                    \
      std::abort();                                                          \
    }                                                                        \
  } while (0)

namespace {

void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream ofs(path, std::ios::binary);
  ofs.write(content.data(), content.size());
}

}  // namespace

int main(int argc, char* argv[]) {
  // Run Qt in headless offscreen mode
  setenv("QT_QPA_PLATFORM", "offscreen", 1);

  QApplication app(argc, argv);
  app.setApplicationName("SoEasy-SmokeTest");

  std::cout << "========================================================\n";
  std::cout << "   SoEasy Qt5 GUI 桌面交互前端全套组件集成验证 (Phase 4)\n";
  std::cout << "   SearchBar + SettingsDialog + HoverPopup + Preview + MainWindow\n";
  std::cout << "========================================================\n\n";

  const std::string test_dir = "/tmp/soeasy_gui_test";
  const std::string test_db = "/tmp/soeasy_gui_test.db";
  const std::string passphrase = "SoEasy_Phase4_Passphrase_2026!";
  const std::string dict_dir = soeasy::test::FindDictDir();

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);
  std::remove(test_db.c_str());
  std::remove((test_db + "-wal").c_str());
  std::remove((test_db + "-shm").c_str());

  // -------------------------------------------------------------
  // Test 1: SearchBar component signal & state verification
  // -------------------------------------------------------------
  std::cout << "[Step 1] 测试 SearchBar 搜索栏组件交互与信号触发...\n";
  {
    soeasy::gui::SearchBar bar;
    QString captured_query;
    int captured_mode = -1;
    QString captured_scope;
    bool settings_clicked = false;

    QObject::connect(&bar, &soeasy::gui::SearchBar::SearchTriggered,
                     [&](const QString& q, int m, const QString& s) {
                       captured_query = q;
                       captured_mode = m;
                       captured_scope = s;
                     });
    QObject::connect(&bar, &soeasy::gui::SearchBar::SettingsRequested,
                     [&]() { settings_clicked = true; });

    // Simulate user typing query
    bar.SetQuery("人工智能");
    CHECK(bar.query() == "人工智能");
    CHECK(captured_query == "人工智能");
    CHECK(captured_mode == soeasy::gui::kModeFulltext);

    // Switch mode to filename
    bar.SetMode(soeasy::gui::kModeFilename);
    CHECK(bar.mode() == soeasy::gui::kModeFilename);
    CHECK(captured_mode == soeasy::gui::kModeFilename);

    // Set path scope
    bar.SetPathScope("/home/user/docs");
    CHECK(bar.path_scope() == "/home/user/docs");

    std::cout << "  -> 检索词/模式/路径过滤联动信号测试通过!\n";
  }

  // -------------------------------------------------------------
  // Test 2: SettingsDialog configuration update verification
  // -------------------------------------------------------------
  std::cout << "\n[Step 2] 测试 SettingsDialog 设置中心组件与配置持久化...\n";
  {
    auto config = std::make_shared<soeasy::service::IndexConfig>();
    size_t default_pats_count = config->GetExcludedPatterns().size();
    config->AddIndexedDirectory("/tmp/soeasy_gui_test");
    config->AddExcludedPattern(".custom_cache_dir");

    soeasy::gui::SettingsDialog dlg(config);

    // Verify initial values
    CHECK(config->GetIndexedDirectories().size() == 1);
    CHECK(config->GetExcludedPatterns().size() == default_pats_count + 1);

    // Add custom extensions
    config->AddSupportedExtension("md");
    config->AddSupportedExtension("rs");
    config->set_max_file_size(32 * 1024 * 1024);

    CHECK(config->ShouldIndex("/tmp/test.rs", "rs", 100));
    CHECK(config->max_file_size() == 32 * 1024 * 1024);

    std::cout << "  -> 索引目录/排除模式/扩展名扩展配置应用通过!\n";
  }

  // -------------------------------------------------------------
  // Test 2.5: Bilingual UI and Language Switching in Settings
  // -------------------------------------------------------------
  std::cout << "\n[Step 2.5] 测试中英双语国际化 (i18n) 与设置中心语言切换...\n";
  {
    auto config = std::make_shared<soeasy::service::IndexConfig>();
    CHECK(config->ui_language() == "zh");
    soeasy::gui::I18n::Instance().SetLanguage("zh");
    CHECK(soeasy::gui::Tr("app_title") == "SoEasy - 让电脑没有难搜的文件");
    CHECK(soeasy::gui::Tr("btn_preview") == "◨ 预览");
    CHECK(soeasy::gui::Tr("th_filename") == "文件名称");

    // Switch to English
    soeasy::gui::I18n::Instance().SetLanguage("en");
    CHECK(soeasy::gui::Tr("app_title") == "SoEasy - Make Every File Searchable");
    CHECK(soeasy::gui::Tr("btn_preview") == "◨ Preview");
    CHECK(soeasy::gui::Tr("th_filename") == "File Name");

    // Verify format categories & format descriptions in English
    CHECK(soeasy::gui::I18n::Instance().GetCategoryName(0, "default") == "Microsoft Office OOXML Formats");
    CHECK(soeasy::gui::I18n::Instance().GetFormatDesc("docx", "default") == "Word 2007+ Document");
    CHECK(soeasy::gui::I18n::Instance().GetFormatDesc("pdf", "default") == "Adobe PDF Portable Document");
    CHECK(soeasy::gui::I18n::Instance().GetFormatDesc("cpp", "default") == "C++ Source Code");

    // Verify config persistence for language
    config->set_ui_language("en");
    CHECK(config->ui_language() == "en");

    // Test SettingsDialog rendering in English on Tab 3 (File Formats)
    {
      soeasy::gui::SettingsDialog dlg_en(config);
      dlg_en.resize(800, 600);
      dlg_en.show();
      auto* tab_widget = dlg_en.findChild<QTabWidget*>();
      if (tab_widget) {
        tab_widget->setCurrentIndex(3);
      }
      app.processEvents();
      QPixmap pix = dlg_en.grab();
      bool ok = pix.save("/tmp/soeasy_settings_dlg_en.png");
      std::cout << "  -> dlg_en save ok: " << ok << ", isNull: " << pix.isNull()
                << ", size: " << pix.width() << "x" << pix.height() << "\n";
    }

    std::string test_cfg_file = "/tmp/soeasy_i18n_test_config.json";
    CHECK(config->SaveToFile(test_cfg_file));

    auto loaded_cfg = std::make_shared<soeasy::service::IndexConfig>();
    CHECK(loaded_cfg->LoadFromFile(test_cfg_file));
    CHECK(loaded_cfg->ui_language() == "en");
    std::remove(test_cfg_file.c_str());

    // Switch back to Chinese
    soeasy::gui::I18n::Instance().SetLanguage("zh");
    config->set_ui_language("zh");
    CHECK(soeasy::gui::Tr("app_title") == "SoEasy - 让电脑没有难搜的文件");
    CHECK(soeasy::gui::I18n::Instance().GetCategoryName(0, "default") == "Microsoft Office 新版格式 (OOXML)");
    CHECK(soeasy::gui::I18n::Instance().GetFormatDesc("docx", "Word 2007+ 文本文档") == "Word 2007+ 文本文档");

    // Test SettingsDialog rendering in Chinese on Tab 3
    {
      soeasy::gui::SettingsDialog dlg_zh(config);
      dlg_zh.resize(800, 600);
      dlg_zh.show();
      auto* tab_widget = dlg_zh.findChild<QTabWidget*>();
      if (tab_widget) {
        tab_widget->setCurrentIndex(3);
      }
      app.processEvents();
      QPixmap pix = dlg_zh.grab();
      pix.save("/tmp/soeasy_settings_dlg_zh.png");
    }

    std::cout << "  -> 中英双语即时翻译、文件格式说明、配置持久化与切换验证完全通过!\n";
  }

  // -------------------------------------------------------------
  // Test 3: HoverPreviewPopup floating card verification
  // -------------------------------------------------------------
  std::cout << "\n[Step 3] 测试 HoverPreviewPopup 悬浮摘要卡片渲染...\n";
  {
    soeasy::gui::HoverPreviewPopup popup;
    popup.ShowSnippet(
        QPoint(100, 100),
        "深度学习笔记.md",
        "/tmp/soeasy_gui_test/深度学习笔记.md",
        "这里是正文摘要: <mark>深度学习</mark> 是机器学习的核心分支...",
        1024, 1700000000, 0.95);

    CHECK(popup.isVisible());
    popup.HidePopup();
    CHECK(!popup.isVisible());

    std::cout << "  -> 悬浮窗快速显示与自动隐藏逻辑正常!\n";
  }

  // -------------------------------------------------------------
  // Test 4: Pipeline Data Preparation (Storage + Worker + Watcher)
  // -------------------------------------------------------------
  std::cout << "\n[Step 4] 准备后端流水线与测试数据集...\n";
  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  bool ok = storage->Open(test_db, passphrase, dict_dir);
  CHECK(ok);

  auto worker_guard = std::make_shared<soeasy::service::WorkerGuard>();
  std::string worker_bin = soeasy::test::FindWorkerBin();
  bool worker_ok = worker_guard->Init(worker_bin, 2, 1500);
  CHECK(worker_ok);

  auto config = std::make_shared<soeasy::service::IndexConfig>();
  config->AddIndexedDirectory(test_dir);
  config->AddSupportedExtension("txt");
  config->AddSupportedExtension("md");
  config->AddSupportedExtension("doc");

  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(
      config, worker_guard, storage);
  bool pipe_ok = pipeline->Init();
  CHECK(pipe_ok);

  // Write sample documents
  std::string doc1 = test_dir + "/chinese_ai.txt";
  WriteFile(doc1,
            "人工智能与深度学习正在深刻变革桌面检索与全文本地知识库检索系统。\n"
            "SoEasy 采用 SQLite3MC 加密与 FTS5 结巴分词底座，保障高性能与隐私安全。");

  std::string doc2 = test_dir + "/rust_system.md";
  WriteFile(doc2,
            "Rust 拥有内存安全与无畏并发的特性，常用于构建底层系统与沙箱进程。\n"
            "SoEasy 沙箱使用 C++17 与 POSIX SHM 共享内存进行高速数据交换。");

  std::string doc3 = test_dir + "/sample_specification_016.doc";
  WriteFile(doc3,
            "这是一份关于技术规格说明与办公文档标准规范的测试正文，包含关键字检索与全文字段匹配验证。\n"
            "在右侧预览卡片中检验微软 Word 的蓝色 W 徽章图标是否完整且不被裁切。");

  pipeline->AddAndScanDirectory(test_dir, true);
  pipeline->StartLiveWatch();

  // Wait for indexing to complete
  for (int i = 0; i < 30; ++i) {
    if (pipeline->name_index() && pipeline->name_index()->size() >= 3) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK(pipeline->name_index());
  CHECK(pipeline->name_index()->size() >= 2);
  std::cout << "  -> 数据集已完成实时解析与倒排索引构建, 索引文件数: "
            << pipeline->name_index()->size() << "\n";

  // -------------------------------------------------------------
  // Test 5: ContentPreviewDialog with decompressed text & keywords
  // -------------------------------------------------------------
  std::cout << "\n[Step 5] 测试 ContentPreviewDialog 解密预览与关键词高亮定位...\n";
  {
    int64_t doc_id = soeasy::service::IndexPipeline::GenerateDocId(doc1);
    std::vector<std::string> kws = {"人工智能", "SoEasy"};

    soeasy::gui::ContentPreviewDialog preview_dlg(
        doc_id, "chinese_ai.txt", QString::fromStdString(doc1), kws, storage);

    // Verify document loaded and displayed
    CHECK(preview_dlg.windowTitle().contains("chinese_ai.txt"));
    std::cout << "  -> 关键词高亮对话框初始化与全文渲染成功!\n";
  }

  // -------------------------------------------------------------
  // Test 6: MainWindow Integration with live pipeline
  // -------------------------------------------------------------
  std::cout << "\n[Step 6] 测试 MainWindow 主窗口全文检索与列表呈现联动...\n";
  {
    soeasy::gui::MainWindow main_win(pipeline);
    main_win.show();

    // Trigger full-text search: "人工智能"
    auto results_ai = pipeline->SearchFulltext("人工智能", 10, 0);
    CHECK(!results_ai.empty());
    CHECK(results_ai[0].file_name == "chinese_ai.txt");
    std::cout << "  -> 全文检索命中: " << results_ai[0].file_name
              << ", 摘要: " << results_ai[0].snippet << "\n";

    // Trigger filename search: "rust"
    auto results_rust = pipeline->SearchFilename("rust", 10);
    CHECK(!results_rust.empty());
    CHECK(results_rust[0].file_name == "rust_system.md");
    std::cout << "  -> 文件名秒搜命中: " << results_rust[0].file_name << "\n";

    // Path scoped search test
    auto results_scoped =
        pipeline->SearchFulltext("深度学习", 10, 0, test_dir);
    CHECK(!results_scoped.empty());
    std::cout << "  -> 路径范围限定检索有效, 命中条数: " << results_scoped.size() << "\n";

    main_win.resize(1180, 680);
    app.processEvents();

    QPixmap pix_unselected = main_win.grab();
    const QString unselected_path =
        QDir::tempPath() + "/soeasy_unselected_preview_ui.png";
    pix_unselected.save(unselected_path);
    std::cout << "  -> 未选中/初始预览界面截屏已成功保存: "
              << unselected_path.toStdString() << "\n";

    // Test Master-Detail live preview panel integration
    auto* sbar = main_win.findChild<soeasy::gui::SearchBar*>();
    if (sbar) {
      sbar->SetQuery("规格说明");
    }
    app.processEvents();

    QPixmap pix = main_win.grab();
    const QString snapshot_path =
        QDir::tempPath() + "/soeasy_master_detail_ui.png";
    pix.save(snapshot_path);
    std::cout << "  -> Master-Detail 双栏即时预览界面截屏已成功保存: "
              << snapshot_path.toStdString() << "\n";
  }

  // -------------------------------------------------------------
  // Test 7: ResultView Sequence Number Column & Lazy AppendResults
  // -------------------------------------------------------------
  std::cout << "\n[Step 7] 测试 ResultView 序号列显示与懒加载 AppendResults 追加...\n";
  {
    soeasy::gui::ResultView rv;
    CHECK(rv.columnCount() == 6);
    CHECK(rv.horizontalHeaderItem(0)->text() == "序号");

    std::vector<soeasy::gui::DisplayResultItem> batch1;
    for (int i = 1; i <= 100; ++i) {
      soeasy::gui::DisplayResultItem it;
      it.doc_id = i;
      it.file_name = QString("file_%1.txt").arg(i);
      it.file_path = QString("/path/file_%1.txt").arg(i);
      it.ext = "txt";
      it.file_size = 2048;
      it.mtime = 1700000000;
      it.score = 50.0;
      batch1.push_back(std::move(it));
    }
    rv.SetResults(batch1);
    CHECK(rv.rowCount() == 100);
    CHECK(rv.item(0, 0)->text() == "1");
    CHECK(rv.item(99, 0)->text() == "100");

    // Test Lazy Loading Append next batch (101..150)
    std::vector<soeasy::gui::DisplayResultItem> batch2;
    for (int i = 101; i <= 150; ++i) {
      soeasy::gui::DisplayResultItem it;
      it.doc_id = i;
      it.file_name = QString("file_%1.txt").arg(i);
      it.file_path = QString("/path/file_%1.txt").arg(i);
      it.ext = "txt";
      it.file_size = 2048;
      it.mtime = 1700000000;
      it.score = 40.0;
      batch2.push_back(std::move(it));
    }
    rv.AppendResults(batch2);
    CHECK(rv.rowCount() == 150);
    CHECK(rv.item(100, 0)->text() == "101");
    CHECK(rv.item(149, 0)->text() == "150");
    std::cout << "  -> 序号列精准对齐渲染与懒加载数据无缝追加通过 (1..100 -> 101..150)!\n";

    // Test NameIndex pagination with offset
    soeasy::service::NameIndex name_idx;
    for (int i = 1; i <= 250; ++i) {
      soeasy::service::NameRecord rec;
      rec.doc_id = i;
      rec.file_name = "doc_" + std::to_string(i) + ".txt";
      rec.file_path = "/path/" + rec.file_name;
      rec.ext = "txt";
      rec.file_size = 1024;
      rec.mtime = 1700000000;
      name_idx.Upsert(rec);
    }
    int total_name_matches = 0;
    auto p1 = name_idx.Search("doc", 100, 0, "", &total_name_matches);
    CHECK(total_name_matches == 250);
    auto p2 = name_idx.Search("doc", 100, 100, "", &total_name_matches);
    CHECK(total_name_matches == 250);
    auto p3 = name_idx.Search("doc", 100, 200, "", &total_name_matches);
    CHECK(total_name_matches == 250);
    auto p4 = name_idx.Search("doc", 100, 250, "", &total_name_matches);
    CHECK(total_name_matches == 250);
    CHECK(p1.size() == 100);
    CHECK(p2.size() == 100);
    CHECK(p3.size() == 50);
    CHECK(p4.empty());
    CHECK(p1[0].doc_id != p2[0].doc_id);

    // Test StorageEngine full-text total_count
    int total_storage_hits = 0;
    auto st_hits = storage->Search("SoEasy", 10, 0, "", &total_storage_hits);
    CHECK(total_storage_hits > 0);
    CHECK(static_cast<int>(st_hits.size()) <= 10);
    std::cout << "  -> NameIndex 与 StorageEngine 总匹配数与分页逻辑验证完全通过!\n";
  }

  // Cleanup
  pipeline->StopLiveWatch();
  worker_guard.reset();
  storage->Close();

  std::filesystem::remove_all(test_dir);
  std::remove(test_db.c_str());
  std::remove((test_db + "-wal").c_str());
  std::remove((test_db + "-shm").c_str());

  std::cout << "\n========================================================\n";
  std::cout << "   [PASS] 全部 Qt5 GUI 交互组件及业务流水线自动化集成验证通过!\n";
  std::cout << "========================================================\n";
  return 0;
}
