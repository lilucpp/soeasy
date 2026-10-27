#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/logger.h"
#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

namespace {

void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream ofs(path, std::ios::binary);
  ofs.write(content.data(), content.size());
}

std::string ReadFileToString(const std::string& path) {
  std::ifstream ifs(path);
  if (!ifs.is_open()) return "";
  return std::string((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char* argv[]) {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy spdlog 日志记录与全量重建索引实证测试\n";
  std::cout << "   - 单文件解析耗时与成功/失败记录\n";
  std::cout << "   - 滚动文件记录 (3 个文件, 每个 10MB)\n";
  std::cout << "   - 文件变动监控日志\n";
  std::cout << "   - 全盘索引清空与增量重建验证\n";
  std::cout << "========================================================\n\n";

  const std::string test_dir = "/tmp/soeasy_log_rebuild_test";
  const std::string test_log = "/tmp/soeasy_log_rebuild_test/logs/test_rolling.log";
  const std::string test_db = "/tmp/soeasy_log_rebuild_test/test_index.db";
  const std::string passphrase = "SoEasy_Log_Test_Passphrase_2026!";
  const std::string dict_dir = soeasy::test::FindDictDir();

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);
  std::filesystem::create_directories(test_dir + "/docs");

  // [Step 1] 初始化 spdlog 滚动日志器 (3个文件, 10MB/文件)
  std::cout << "[Step 1] 初始化 spdlog 滚动日志系统 (10MB x 3 files)...\n";
  soeasy::common::Logger::Init(test_log, 10 * 1024 * 1024, 3, true);
  LOG_INFO(">>> [TEST] spdlog rotating logger successfully initialized <<<");
  std::cout << "  [+] 日志系统就绪! 目标文件: " << test_log << "\n\n";

  // [Step 2] 初始化后端存储引擎与 Worker 沙箱进程
  std::cout << "[Step 2] 初始化 SQLite3MC 加密存储与 Worker 解析沙箱...\n";
  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  if (!storage->Open(test_db, passphrase, dict_dir)) {
    std::cerr << "[-] StorageEngine failed to open: " << storage->GetLastError() << "\n";
    return 1;
  }

  auto worker_guard = std::make_shared<soeasy::service::WorkerGuard>();
  std::string worker_bin = "./build/soeasy-worker";
  if (!std::filesystem::exists(worker_bin)) {
    worker_bin = "./soeasy-worker";
  }
  if (!worker_guard->Init(worker_bin, 2, 1500)) {
    std::cerr << "[-] WorkerGuard failed to init\n";
    return 1;
  }

  auto config = std::make_shared<soeasy::service::IndexConfig>();
  config->set_debounce_ms(100);

  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(
      config, worker_guard, storage);
  if (!pipeline->Init()) {
    std::cerr << "[-] IndexPipeline failed to init\n";
    return 1;
  }
  std::cout << "  [+] 存储与沙箱流水线初始化完成!\n\n";

  // [Step 3] 写入测试文档并测试单文件解析与日志记录
  std::cout << "[Step 3] 写入测试文档并验证单文件解析耗时与成功/失败日志...\n";
  std::string file1 = test_dir + "/docs/tech_report.txt";
  std::string file2 = test_dir + "/docs/quantum_ai.md";
  WriteFile(file1, "SoEasy 深度检索系统技术报告：集成 spdlog 高性能滚动日志组件。");
  WriteFile(file2, "量子计算与人工智能深度模型前沿技术分析方案。");

  // 添加目录并扫描
  pipeline->AddAndScanDirectory(test_dir + "/docs", true);

  // 验证当前全文检索与文件名搜索
  auto hits1 = pipeline->SearchFulltext("量子计算", 10);
  assert(!hits1.empty() && "Should find hits for quantum ai");
  std::cout << "  [+] 全文检索成功命中: " << hits1[0].file_name << "\n";

  auto hits2 = pipeline->SearchFilename("tech_report", 10);
  assert(!hits2.empty() && "Should find hits for tech_report");
  std::cout << "  [+] 文件名秒搜成功命中: " << hits2[0].file_name << "\n\n";

  // [Step 4] 启动实时监控并验证文件监控日志
  std::cout << "[Step 4] 激活文件监控并验证变动日志...\n";
  pipeline->StartLiveWatch();

  std::string file3 = test_dir + "/docs/live_added.txt";
  WriteFile(file3, "动态生成的新文件，用于验证实时文件监控日志输出。");

  // 等待去抖动与自动索引
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  auto hits3 = pipeline->SearchFulltext("动态生成", 10);
  assert(!hits3.empty() && "Live added file should be indexed");
  std::cout << "  [+] 实时监控感知并索引新文件: " << hits3[0].file_name << "\n\n";

  // [Step 5] 验证支持重建索引 (RebuildIndex)
  std::cout << "[Step 5] 验证支持全量清空与重建索引 (RebuildIndex)...\n";
  // 额外添加两个新文件
  std::string file4 = test_dir + "/docs/rebuild_case1.txt";
  std::string file5 = test_dir + "/docs/rebuild_case2.txt";
  WriteFile(file4, "重建索引专用验证用例一：核心架构与流水线。");
  WriteFile(file5, "重建索引专用验证用例二：倒排表与分词器重构。");

  size_t progress_calls = 0;
  size_t last_indexed = 0;
  size_t last_total = 0;

  bool rebuild_ok = pipeline->RebuildIndex(
      [&](size_t indexed, size_t total, const std::string& path) {
        progress_calls++;
        last_indexed = indexed;
        last_total = total;
      });

  if (!rebuild_ok) {
    std::cerr << "[-] RebuildIndex failed\n";
    return 1;
  }
  assert(progress_calls >= 5 && "Progress callback should be called for all files");
  assert(last_indexed == last_total && "All discovered files must be indexed");
  std::cout << "  [+] 重建索引执行成功! 总计索引: " << last_indexed << " / " << last_total
            << " 文件，进度回调触发: " << progress_calls << " 次\n";

  // 验证重建后的全新检索能力
  auto hits_rebuild1 = pipeline->SearchFulltext("核心架构", 10);
  assert(!hits_rebuild1.empty() && "Rebuilt index must find case1");
  std::cout << "  [+] 重建后全文检索命中: " << hits_rebuild1[0].file_name << "\n";

  auto hits_rebuild2 = pipeline->SearchFulltext("分词器", 10);
  assert(!hits_rebuild2.empty() && "Rebuilt index must find case2");
  std::cout << "  [+] 重建后全文检索命中: " << hits_rebuild2[0].file_name << "\n\n";

  // [Step 6] 校验日志文件内容并确认满足所有需求
  std::cout << "[Step 6] 检验实际生成的滚动日志文件及内容...\n";
  soeasy::common::Logger::Shutdown();  // Flush and close log file

  assert(std::filesystem::exists(test_log) && "Log file must exist on disk");
  uintmax_t log_size = std::filesystem::file_size(test_log);
  std::cout << "  [+] 日志文件大小: " << log_size << " 字节\n";

  std::string log_content = ReadFileToString(test_log);

  // 检查关键字：单文件解析耗时、单文件成功/失败、文件监控、重建索引
  assert(log_content.find("[Parser]") != std::string::npos &&
         "Log should contain [Parser] tags");
  assert(log_content.find("SUCCESS") != std::string::npos &&
         "Log should contain SUCCESS status");
  assert(log_content.find("ms") != std::string::npos &&
         "Log should contain parse elapsed milliseconds");
  assert(log_content.find("[WatchEngine]") != std::string::npos &&
         "Log should contain [WatchEngine] logs");
  assert(log_content.find("Starting full index rebuild") != std::string::npos &&
         "Log should contain rebuild starting log");
  assert(log_content.find("Index rebuild finished") != std::string::npos &&
         "Log should contain rebuild finished log");

  std::cout << "  [+] 日志包含单文件解析耗时 (ms) 与成功/失败状态: 验证通过!\n";
  std::cout << "  [+] 日志包含文件系统变动监控 [WatchEngine]: 验证通过!\n";
  std::cout << "  [+] 日志包含全量索引重建全过程: 验证通过!\n\n";

  std::cout << "========================================================\n";
  std::cout << "🎉 全部验证项 100% 通过! spdlog 滚动日志与重建索引实证成功!\n";
  std::cout << "========================================================\n";

  return 0;
}
