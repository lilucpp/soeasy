#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <fstream>

#include "gui/main_window.h"
#include "gui/theme_manager.h"
#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  QApplication app(argc, argv);
  soeasy::gui::ThemeManager::Instance().Init(&app);

  const std::string test_dir = soeasy::test::GetTestTempPath("soeasy_capture_test").string();
  const std::string test_db = soeasy::test::GetTestTempPath("soeasy_capture_test.db").string();
  const std::string passphrase = "Capture_Passphrase_2026!";
  const std::string dict_dir = soeasy::test::FindDictDir();

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);
  std::remove(test_db.c_str());

  // Create sample files strictly matching Figma Demo
  {
    std::ofstream ofs(test_dir + "/test2.sh");
    ofs << "#!/bin/bash\n\n# ==================================================\n# 待测试文件大小（建议大于物理内存的 1/4，或者至少 1GB）\nFILE_SIZE=\"2G\"\nSOURCE_FILE=\"test_data.bin\"\nDEST_SYS=\"test_data.sys\"\nDEST_MY=\"test_data.my\"\n\n# 你的程序路径\nMY_CMD=\"./my_copy\" # 请修改为你编译好的程序名\n\n# 确保 sudo 权限可用（清空缓存需要）\nif [ \"$EUID\" -ne 0 ]; then\n  echo \"请使用 sudo 运行此脚本，以便在测试前清空缓存。\"\n  exit\nfi\n\n# ================== 1. 数据准备 ==================\necho \"----------------------------------------------\"\necho \"\\正在准备测试数据（大小: $FILE_SIZE）...\"\necho \"使用 /dev/urandom 防止稀疏文件优化\"\necho \"----------------------------------------------\"\n";
  }
  {
    std::ofstream ofs(test_dir + "/google_antigravity_guide.md");
    ofs << "# Antigravity Guide\n测试智能体与桌面搜索深度集成规范。\n";
  }
  {
    std::ofstream ofs(test_dir + "/main.cpp");
    ofs << "#include <iostream>\n// 测试用例 02：多线程并行计算测试\nint main() {\n  std::cout << \"测试并发流水线运行正常\" << std::endl;\n  return 0;\n}\n";
  }
  {
    std::ofstream ofs(test_dir + "/linux_kernel_optimize.md");
    ofs << "# Linux Performance Optimization\n测试页缓存与 IO 性能调优技术指南。\n";
  }

  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  storage->Open(test_db, passphrase, dict_dir);

  auto config = std::make_shared<soeasy::service::IndexConfig>();
  config->AddIndexedDirectory(test_dir);
  config->set_dict_dir(dict_dir);

  auto worker_guard = std::make_shared<soeasy::service::WorkerGuard>();
  worker_guard->Init("./soeasy-worker", 2, 10000);

  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(config, worker_guard, storage);
  pipeline->Init();
  pipeline->PerformInitialScan();

  // Wait a moment for index
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  soeasy::gui::MainWindow main_win(pipeline);
  main_win.resize(1360, 900);
  main_win.show();
  app.processEvents();

  // Grab initial state (default startup)
  QPixmap pix_init = main_win.grab();
  pix_init.save("/home/peanut/.gemini/antigravity/brain/29d8fd88-daba-4851-98ff-71031948fbd4/soeasy_real_startup_current.png");
  std::cout << "[+] Saved startup capture to soeasy_real_startup_current.png\n";

  // Trigger search "测试" to test result card and preview card
  auto *sbar = main_win.findChild<soeasy::gui::SearchBar *>();
  auto *rview = main_win.findChild<soeasy::gui::ResultView *>();
  if (sbar) {
    sbar->SetQuery("测试");
  }
  app.processEvents();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  app.processEvents();

  if (rview && rview->rowCount() > 0) {
    rview->selectRow(0);
  }
  app.processEvents();

  QPixmap pix_search = main_win.grab();
  pix_search.save("/home/peanut/.gemini/antigravity/brain/29d8fd88-daba-4851-98ff-71031948fbd4/soeasy_real_search_rendered.png");
  std::cout << "[+] Saved search capture to soeasy_real_search_rendered.png\n";

  return 0;
}
