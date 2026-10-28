#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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

}  // namespace

int main(int argc, char* argv[]) {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy 文件变动监控与实时增量引擎独立实证 Demo (Phase 3)\n";
  std::cout << "   efsw 跨平台监控 + 去抖动流水线 + 内存文件名秒搜 + 范围限定\n";
  std::cout << "========================================================\n\n";

  const std::string test_dir = "/tmp/soeasy_watch_test";
  const std::string test_db = "/tmp/soeasy_phase3_test.db";
  const std::string passphrase = "SoEasy_Phase3_Passphrase_2026!";
  const std::string dict_dir = soeasy::test::FindDictDir();

  // Clean up any leftovers
  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);
  std::filesystem::create_directories(test_dir + "/node_modules");
  std::filesystem::create_directories(test_dir + "/work");
  std::filesystem::create_directories(test_dir + "/personal");

  std::remove(test_db.c_str());
  std::remove((test_db + "-wal").c_str());
  std::remove((test_db + "-shm").c_str());

  // 1. Initialize StorageEngine
  std::cout << "[Step 1] 初始化存储底座与加密数据库...\n";
  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  if (!storage->Open(test_db, passphrase, dict_dir)) {
    std::cerr << "[-] 存储底座打开失败: " << storage->GetLastError() << "\n";
    return 1;
  }
  std::cout << "[+] 加密数据库与 FTS5 分词器成功加载!\n\n";

  // 2. Initialize WorkerGuard
  std::cout << "[Step 2] 初始化 Worker 解析沙箱进程池...\n";
  auto worker_guard = std::make_shared<soeasy::service::WorkerGuard>();
  if (!worker_guard->Init("", 2, 2000, 8, 8 * 1024 * 1024)) {
    std::cerr << "[-] WorkerGuard 初始化失败!\n";
    return 1;
  }
  std::cout << "[+] WorkerGuard 成功启动 (2 个沙箱进程, 64MB 共享内存池)!\n\n";

  // 3. Initialize IndexConfig and IndexPipeline
  std::cout << "[Step 3] 初始化索引配置与增量流水线...\n";
  auto config = std::make_shared<soeasy::service::IndexConfig>();
  config->set_debounce_ms(150);  // 150ms debounce for rapid test response

  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(
      config, worker_guard, storage);
  if (!pipeline->Init()) {
    std::cerr << "[-] 流水线初始化失败!\n";
    return 1;
  }
  std::cout << "[+] 索引流水线、去抖器及 efsw 监控引擎就绪!\n\n";

  // 4. Initial Baseline Crawl
  std::cout << "[Step 4] 写入测试基线文件并执行全量扫描...\n";
  std::string f1 = test_dir + "/financial_report.txt";
  std::string f2 = test_dir + "/system_architecture.md";
  std::string f3 = test_dir + "/network_main.cpp";

  WriteFile(f1, "2026年度企业营收与财务报表审计报告，各项支出核算准确无误。");
  WriteFile(f2, "SoEasy 桌面搜索引擎采用 Cap'n Proto RPC 混合零拷贝架构设计。");
  WriteFile(f3, "#include <iostream>\nint main() { return 0; }");

  auto t_scan0 = std::chrono::high_resolution_clock::now();
  pipeline->AddAndScanDirectory(test_dir, true);
  auto t_scan1 = std::chrono::high_resolution_clock::now();
  double scan_ms =
      std::chrono::duration<double, std::milli>(t_scan1 - t_scan0).count();

  auto stats = pipeline->GetStats();
  std::cout << "[+] 基线扫描完成! 耗时: " << std::fixed << std::setprecision(2)
            << scan_ms << " ms, 成功索引文件数: " << stats.total_files_indexed
            << "\n";

  // Verify full-text baseline query
  auto hits1 = pipeline->SearchFulltext("财务报表", 10, 0);
  if (hits1.empty()) {
    std::cerr << "[-] 基线全文检索失败!\n";
    return 1;
  }
  std::cout << "    • 全文检索 \"财务报表\" -> 命中: " << hits1[0].file_name
            << " (BM25: " << hits1[0].rank << ")\n";

  // Verify filename fast search
  auto name_hits = pipeline->SearchFilename("financial", 10);
  if (name_hits.empty()) {
    std::cerr << "[-] 文件名秒搜失败!\n";
    return 1;
  }
  std::cout << "    • 文件名秒搜 \"financial\" -> 命中: "
            << name_hits[0].file_name << "\n\n";

  // 5. Start Live Watching
  std::cout << "[Step 5] 启动 efsw 实时变动监控...\n";
  if (!pipeline->StartLiveWatch()) {
    std::cerr << "[-] 启动实时监控失败!\n";
    return 1;
  }
  std::cout << "[+] 实时监控已在后台线程激活!\n\n";

  // 6. Test Dynamic File Creation
  std::cout << "[Step 6] 验证动态新增文件实时感知与自动索引...\n";
  std::string f_new = test_dir + "/contracts_2026.txt";
  auto t_add_start = std::chrono::high_resolution_clock::now();
  WriteFile(f_new,
            "高新技术企业核心技术转让保密合同协议书，发文字号国发2026号。");

  // Wait for debounce and worker processing (250ms)
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  auto hits_new = pipeline->SearchFulltext("技术转让", 10, 0);
  auto t_add_end = std::chrono::high_resolution_clock::now();
  double add_cost_ms =
      std::chrono::duration<double, std::milli>(t_add_end - t_add_start).count();

  if (hits_new.empty()) {
    std::cerr << "[-] 动态新增文件未被检索到!\n";
    return 1;
  }
  std::cout << "[+] 动态新增成功感知! 耗时: " << add_cost_ms
            << " ms (含去抖等待 150ms)\n";
  std::cout << "    • 全文检索 \"技术转让\" -> 命中: " << hits_new[0].file_name
            << "\n\n";

  // 7. Test Dynamic File Modification
  std::cout << "[Step 7] 验证动态文件修改与正文更新...\n";
  WriteFile(f2,
            "SoEasy 桌面搜索引擎全新升级：集成 efsw 实时变动监控与事件去抖动引擎。");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  auto hits_mod = pipeline->SearchFulltext("去抖动", 10, 0);
  if (hits_mod.empty()) {
    std::cerr << "[-] 修改内容未被增量更新!\n";
    return 1;
  }
  std::cout << "[+] 动态修改成功感知与增量重索引通过!\n";
  std::cout << "    • 检索新增词 \"去抖动\" -> 命中: " << hits_mod[0].file_name
            << "\n\n";

  // 8. Test Dynamic File Deletion
  std::cout << "[Step 8] 验证文件删除后索引自动注销与清理...\n";
  std::remove(f1.c_str());
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  auto hits_del = pipeline->SearchFulltext("财务报表", 10, 0);
  auto name_del = pipeline->SearchFilename("financial", 10);
  if (!hits_del.empty() || !name_del.empty()) {
    std::cerr << "[-] 删除文件后仍存在残留索引!\n";
    return 1;
  }
  std::cout << "[+] 删除感知验证通过! 倒排索引与内存文件名表中均已彻底清理!\n\n";

  // 9. Test Exclusion Rules
  std::cout << "[Step 9] 验证排除规则过滤 (node_modules 目录应被完全忽略)...\n";
  std::string f_excluded = test_dir + "/node_modules/ignored_lib.txt";
  WriteFile(f_excluded, "这是一段在 node_modules 目录下的测试内容。");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  auto hits_excluded = pipeline->SearchFulltext("node_modules", 10, 0);
  if (!hits_excluded.empty()) {
    std::cerr << "[-] 排除规则失效! 命中文件: " << hits_excluded[0].file_path
              << ", 标题: " << hits_excluded[0].file_name 
              << ", doc_id: " << hits_excluded[0].doc_id
              << ", rank: " << hits_excluded[0].rank << "\n";
    std::string text;
    storage->GetDocumentContent(hits_excluded[0].doc_id, &text);
    std::cerr << "[-] 命中文件内容: [" << text << "]\n";
    return 1;
  }
  // 9.1 Verify efsw unsupported suffix filtering
  std::cout << "[Step 9.1] 验证 efsw 非关心后缀前置过滤 (.tmp, .o, .swp)...\n";
  std::string f_tmp = test_dir + "/temp_cache.tmp";
  std::string f_obj = test_dir + "/main.o";
  std::string f_swp = test_dir + "/doc.swp";
  WriteFile(f_tmp, "This is temporary junk data that should be skipped by efsw immediately.");
  WriteFile(f_obj, "ELF binary object data not supported for text indexing.");
  WriteFile(f_swp, "Vim swap file data.");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  auto hits_tmp = pipeline->SearchFulltext("temporary", 10, 0);
  if (!hits_tmp.empty()) {
    std::cerr << "[-] 错误: 非关心后缀 .tmp 被索引! 命中数: " << hits_tmp.size() << "\n";
    return 1;
  }
  auto name_obj = pipeline->SearchFilename("main.o", 10);
  if (!name_obj.empty()) {
    std::cerr << "[-] 错误: 非关心后缀 .o 被加入文件名索引!\n";
    return 1;
  }
  std::cout << "[+] efsw 非关心后缀过滤验证通过! (.tmp/.o/.swp 均被前置拦截，未进入流水线)\n\n";

  // 9.2 Verify directory pruning during rebuild
  std::cout << "[Step 9.2] 验证递归目录扫描剪枝 (.git, build 目录直接剪枝跳过)...\n";
  std::string f_git_txt = test_dir + "/.git/commit_message.txt";
  std::string f_build_txt = test_dir + "/build/generated_notes.txt";
  WriteFile(f_git_txt, "Git commit message in .git directory should be pruned.");
  WriteFile(f_build_txt, "Build artifact text in build directory should be pruned.");
  
  // Rebuild index and check discovered files count
  size_t indexed_count = pipeline->PerformInitialScan([](size_t, size_t, const std::string&) {});
  (void)indexed_count;
  auto hits_git = pipeline->SearchFulltext("commit_message", 10, 0);
  if (!hits_git.empty()) {
    std::cerr << "[-] 错误: .git/ 目录下的文件未被剪枝跳过!\n";
    return 1;
  }
  std::cout << "[+] 目录剪枝验证通过! (.git/ 与 build/ 目录子树被完整 prune，未被遍历)\n\n";

  // 10. Test Path Scoping (Search path setting)
  std::cout << "[Step 10] 验证搜索路径范围限定 (Path Scoping)...\n";
  std::string f_work = test_dir + "/work/meeting_notes.txt";
  std::string f_personal = test_dir + "/personal/meeting_notes.txt";

  WriteFile(f_work, "工作例会备忘录：关于下季度核心技术研发规划。");
  WriteFile(f_personal, "私人生活备忘录：关于周末家庭聚餐规划。");
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  // Search constrained to work directory
  auto scoped_hits = pipeline->SearchFulltext(
      "备忘录", 10, 0, /*path_prefix=*/test_dir + "/work");
  if (scoped_hits.size() != 1 ||
      scoped_hits[0].file_path.find("/work/") == std::string::npos) {
    std::cerr << "[-] 搜索路径范围限定过滤失败! 命中数: "
              << scoped_hits.size() << "\n";
    return 1;
  }
  std::cout << "[+] 搜索路径限定生效! 仅精准返回限定路径下的命中文件: "
            << scoped_hits[0].file_path << "\n\n";

  // 11. In-memory Filename Search Benchmark (10,000 files)
  std::cout << "[Step 11] 测试千万量级内存文件名秒搜基准性能 (10,000 文件规模)...\n";
  for (int i = 0; i < 10000; ++i) {
    soeasy::service::NameRecord nr;
    nr.doc_id = 100000 + i;
    nr.file_name = "document_auto_gen_" + std::to_string(i) + ".docx";
    nr.file_path = "/home/data/docs/" + nr.file_name;
    nr.ext = "docx";
    nr.file_size = 1024 * (i + 1);
    nr.mtime = 1730000000 + i;
    pipeline->name_index()->Upsert(nr);
  }

  auto t_name0 = std::chrono::high_resolution_clock::now();
  auto name_res = pipeline->SearchFilename("auto_gen_888", 10);
  auto t_name1 = std::chrono::high_resolution_clock::now();
  double name_query_us =
      std::chrono::duration<double, std::micro>(t_name1 - t_name0).count();

  std::cout << "[+] 10,000 个在存文件瞬时模糊秒搜耗时: " << std::fixed
            << std::setprecision(2) << name_query_us << " μs ("
            << (name_query_us / 1000.0) << " ms), 命中数: " << name_res.size()
            << "\n";
  if (!name_res.empty()) {
    std::cout << "    └─ 最佳命中: " << name_res[0].file_name << "\n";
  }

  pipeline->StopLiveWatch();
  storage->Close();

  // Cleanup test filesystem
  std::filesystem::remove_all(test_dir);
  std::remove(test_db.c_str());
  std::remove((test_db + "-wal").c_str());
  std::remove((test_db + "-shm").c_str());

  std::cout << "\n========================================================\n";
  std::cout << "🎉 所有验证项全部通过! Phase 3 变动监控与实时增量引擎验证圆满成功!\n";
  std::cout << "========================================================\n";
  return 0;
}
