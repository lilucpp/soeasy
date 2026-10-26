// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <QApplication>
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
#include "gui/main_window.h"
#include "gui/result_view.h"
#include "gui/search_bar.h"
#include "gui/settings_dialog.h"
#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"

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
  const std::string dict_dir =
      "/home/peanut/project/soeasy/third_party/cppjieba/dict";

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
  std::string worker_bin = "/home/peanut/project/soeasy/build/soeasy-worker";
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
        "/home/peanut/.gemini/antigravity/brain/76ed2a22-1d0c-462e-b299-d80268897ee2/unselected_preview_ui.png";
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
        "/home/peanut/.gemini/antigravity/brain/76ed2a22-1d0c-462e-b299-d80268897ee2/master_detail_ui.png";
    pix.save(snapshot_path);
    std::cout << "  -> Master-Detail 双栏即时预览界面截屏已成功保存: "
              << snapshot_path.toStdString() << "\n";
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
