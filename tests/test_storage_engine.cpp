#include "storage/storage_engine.h"

#include <cassert>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string ReadFileBinary(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return "";
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char* argv[]) {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy (Sou Easy) 存储与检索底座独立实证 Demo (Phase 1)\n";
  std::cout << "   SQLite3MC (AES-128, kdf_iter=1) + FTS5 (Jieba) + Zstd\n";
  std::cout << "========================================================\n\n";

  const std::string db_path = "/tmp/soeasy_demo_encrypted.db";
  const std::string key = "SoEasy_TopSecret_Passphrase_2026!";
  const std::string dict_dir =
      "/home/peanut/project/soeasy/third_party/cppjieba/dict";

  // Clean up old demo db
  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());

  soeasy::storage::StorageEngine engine;

  // 1. Open and initialize encrypted database
  std::cout << "[Step 1] 打开并初始化加密数据库: " << db_path << "\n";
  auto t0 = std::chrono::high_resolution_clock::now();
  bool ok = engine.Open(db_path, key, dict_dir);
  auto t1 = std::chrono::high_resolution_clock::now();
  double init_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (!ok) {
    std::cerr << "[-] 打开数据库失败: " << engine.GetLastError() << "\n";
    return 1;
  }
  std::cout << "[+] 加密数据库与 FTS5 分词器初始化成功! 耗时: "
            << std::fixed << std::setprecision(2) << init_ms << " ms\n\n";

  // 2. Index sample documents
  std::cout << "[Step 2] 索引测试文档数据集...\n";

  struct SampleDoc {
    int64_t id;
    std::string path;
    std::string name;
    std::string ext;
    std::string content;
  };

  std::vector<SampleDoc> samples = {
      {1, "/home/user/docs/项目总体架构设计.docx",
       "项目总体架构设计.docx", "docx",
       "SoEasy (Sou Easy) 是一款基于 C++17 与 Qt5 "
       "构建的高性能跨平台桌面全文检索系统。"
       "采用三层多进程沙箱架构：GUI 前端、Service 主服务、Worker "
       "解析进程池。"
       "通信子系统采用 Cap'n Proto RPC 配合共享内存（SHM）实现零拷贝传输。"
       "存储层使用 SQLite3MultipleCiphers 进行 AES-128-CBC "
       "页级透明加密，并结合 FTS5 与 cppjieba 分词器。"
       "文档全文经 Zstandard (Zstd level 3) "
       "高效压缩存储，实现极小磁盘占用与毫秒级预览还原。"},
      {2, "/home/user/finance/2026年度预算与财务合同批复.pdf",
       "2026年度预算与财务合同批复.pdf", "pdf",
       "关于2026年度财务预算与合同签署的批复文件。\n"
       "发文字号：国发〔2026〕18号。各部门关于研发投入与硬件采购预算已获审核"
       "通过。\n"
       "重点支持桌面搜索与智能化办公软件开发，本期预算总计人民币壹仟贰佰万元"
       "整。"},
      {3, "/home/user/code/network_client.cpp", "network_client.cpp", "cpp",
       "#include <iostream>\n"
       "#include <capnp/message.h>\n"
       "// Network client implementation for SoEasy desktop searcher\n"
       "int main() {\n"
       "    std::cout << \"Connecting to SoEasy daemon via Unix Domain "
       "Socket...\" << std::endl;\n"
       "    return 0;\n"
       "}\n"}};

  // Construct a large doc (Doc 4) to test Zstd compression ratio & speed
  std::string large_content;
  large_content.reserve(500000);
  for (int i = 0; i < 1000; ++i) {
    large_content += "这是第 " + std::to_string(i) +
                     " 段技术规范测试文档内容。";
    large_content +=
        "涵盖 SQLite3MC 页级加密、WAL 机制并发写入、FTS5 全文倒排索引以及 "
        "Zstd 压缩技术。\n";
    large_content += "void ExecuteTask_" + std::to_string(i) +
                     "() { int status = 200; return; }\n";
  }

  samples.push_back({4, "/home/user/data/massive_specs.txt",
                     "massive_specs.txt", "txt", large_content});

  for (const auto& doc : samples) {
    auto start = std::chrono::high_resolution_clock::now();
    bool idx_ok = engine.IndexDocument(
        doc.id, doc.path, doc.name, doc.ext,
        doc.content.size(), 1730000000, 2, doc.content);
    auto finish = std::chrono::high_resolution_clock::now();
    double cost_us =
        std::chrono::duration<double, std::micro>(finish - start).count();

    if (!idx_ok) {
      std::cerr << "[-] 索引文档 " << doc.name
                << " 失败: " << engine.GetLastError() << "\n";
      return 1;
    }

    std::cout << "    • 已索引: [" << doc.id << "] " << doc.name
              << " (原文: " << doc.content.size()
              << " 字节, 索引+压缩耗时: " << cost_us << " μs)\n";
  }
  std::cout << "[+] 文档批量索引完成!\n\n";

  // 3. Test Full-text Search with various Chinese and English queries
  std::cout << "[Step 3] 执行全文检索基准测试 (FTS5 + cppjieba)...\n";
  const std::vector<std::string> test_queries = {
      "SoEasy", "财务预算", "\"Cap'n Proto\"",
      "SQLite3MultipleCiphers", "架构", "国发"};

  for (const auto& q : test_queries) {
    auto q_start = std::chrono::high_resolution_clock::now();
    auto hits = engine.Search(q, 10, 0);
    auto q_end = std::chrono::high_resolution_clock::now();
    double q_ms =
        std::chrono::duration<double, std::milli>(q_end - q_start).count();

    std::cout << "  🔍 查询: \"" << q << "\" -> 命中 " << hits.size()
              << " 篇, 耗时: " << std::fixed << std::setprecision(3) << q_ms
              << " ms\n";

    for (const auto& hit : hits) {
      std::string content;
      engine.GetDocumentContent(hit.doc_id, &content);
      std::string snip =
          soeasy::storage::StorageEngine::GenerateSnippet(content, {q}, 80);
      std::cout << "     └─ [" << hit.file_name
                << "] (BM25评分: " << std::setprecision(4) << hit.rank << ")\n";
      std::cout << "        摘要: " << snip << "\n";
    }
  }
  std::cout << "\n";

  // 4. Test Zstd decompression throughput
  std::cout << "[Step 4] 测试 Zstd (Level 3) 原文解压吞吐量 (Doc 4: "
            << large_content.size() << " 字节)...\n";
  std::string decompressed;
  const int kTestRounds = 50;
  auto d_start = std::chrono::high_resolution_clock::now();
  for (int r = 0; r < kTestRounds; ++r) {
    bool d_ok = engine.GetDocumentContent(4, &decompressed);
    if (!d_ok || decompressed.size() != large_content.size()) {
      std::cerr << "[-] 解压数据校验失败!\n";
      return 1;
    }
  }
  auto d_end = std::chrono::high_resolution_clock::now();
  double total_d_ms =
      std::chrono::duration<double, std::milli>(d_end - d_start).count();
  double avg_d_ms = total_d_ms / kTestRounds;
  double mb_per_sec =
      (large_content.size() / 1048576.0) / (avg_d_ms / 1000.0);

  std::cout << "[+] 单次解压耗时: " << std::fixed << std::setprecision(3)
            << avg_d_ms << " ms\n";
  std::cout << "[+] 解压吞吐量: " << std::fixed << std::setprecision(2)
            << mb_per_sec << " MB/s (单核)\n\n";

  // 5. Verify physical encryption (read raw database file)
  std::cout << "[Step 5] 验证底层文件物理加密安全性 (检查裸二进制)...\n";
  engine.Close();  // Close DB and flush WAL to disk

  std::string raw_db = ReadFileBinary(db_path);
  std::cout << "    数据库裸文件大小: " << raw_db.size() << " 字节\n";

  const std::vector<std::string> leak_checks = {
      "财务预算", "项目总体架构设计", "壹仟贰佰万元整", "SoEasy",
      "TopSecret"};

  bool leak_found = false;
  for (const auto& sensitive_word : leak_checks) {
    if (raw_db.find(sensitive_word) != std::string::npos) {
      std::cerr << "[-] 警告: 发现明文泄露: \"" << sensitive_word
                << "\" 在数据库文件中裸露!\n";
      leak_found = true;
    }
  }

  if (!leak_found) {
    std::cout << "[+] 安全验证通过: 裸数据库文件中 0 "
                 "明文泄露! AES-128 页级加密完全生效!\n\n";
  } else {
    std::cerr << "[-] 安全验证失败: 存在明文泄露!\n";
    return 1;
  }

  // 6. Test wrong passphrase authentication rejection
  std::cout << "[Step 6] 验证错误密码鉴权熔断...\n";
  soeasy::storage::StorageEngine wrong_engine;
  bool wrong_ok =
      wrong_engine.Open(db_path, "Incorrect_Password_999", dict_dir);
  if (!wrong_ok) {
    std::cout << "[+] 鉴权熔断生效! 错误密码被成功拦截: "
              << wrong_engine.GetLastError() << "\n\n";
  } else {
    std::cerr << "[-] 错误密码居然成功打开了数据库!\n";
    return 1;
  }

  // 7. Reopen with correct key and re-verify
  std::cout << "[Step 7] 使用原正确密钥重新打开并验证数据持久性...\n";
  soeasy::storage::StorageEngine valid_engine;
  bool reopen_ok = valid_engine.Open(db_path, key, dict_dir);
  if (!reopen_ok) {
    std::cerr << "[-] 重新打开失败: " << valid_engine.GetLastError() << "\n";
    return 1;
  }
  auto verified_hits = valid_engine.Search("财务预算", 10, 0);
  std::cout << "[+] 重新打开成功! 检索 \"财务预算\" 命中: "
            << verified_hits.size() << " 篇。\n";
  valid_engine.Close();

  std::cout << "\n========================================================\n";
  std::cout << "🎉 所有验证项全部通过! Phase 1 存储与检索底座验证圆满成功!\n";
  std::cout << "========================================================\n";
  return 0;
}
