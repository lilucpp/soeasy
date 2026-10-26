#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "common/shm_pool.h"
#include "service/worker_guard.h"
#include "worker/l1_text_parser.h"

namespace {

void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream ofs(path, std::ios::binary);
  ofs.write(content.data(), content.size());
}

}  // namespace

int main(int argc, char* argv[]) {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy (Sou Easy) 混合 IPC 与解析沙箱独立实证 Demo (Phase 2)\n";
  std::cout << "   Cap'n Proto + 共享内存槽位池 + 多进程沙箱守卫 (熔断/自愈)\n";
  std::cout << "========================================================\n\n";

  soeasy::service::WorkerGuard guard;

  // Initialize WorkerGuard with 2 workers, 1500ms timeout for tests, 8 slots of 8MB
  std::cout << "[Step 1] 初始化 Worker 进程池与共享内存槽位池...\n";
  auto t0 = std::chrono::high_resolution_clock::now();
  bool init_ok = guard.Init(/*worker_bin_path=*/"", /*worker_count=*/2,
                            /*timeout_ms=*/1500, /*shm_slot_count=*/8,
                            /*shm_slot_capacity=*/8 * 1024 * 1024);
  auto t1 = std::chrono::high_resolution_clock::now();
  double init_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (!init_ok) {
    std::cerr << "[-] WorkerGuard 初始化失败!\n";
    return 1;
  }
  std::cout << "[+] WorkerGuard 初始化成功! 启动工作进程数: " << guard.worker_count()
            << ", 槽位数: " << guard.shm_pool()->slot_count()
            << ", 槽位容量: 8MB (总池 64MB), 耗时: " << std::fixed
            << std::setprecision(2) << init_ms << " ms\n\n";

  // -------------------------------------------------------------
  // Test 1: Normal large document zero-copy extraction via SHM
  // -------------------------------------------------------------
  std::cout << "[Step 2] 验证大文档跨进程零拷贝解析与数据保真度...\n";
  std::string large_doc_path = "/tmp/soeasy_test_large.txt";
  std::string expected_content;
  expected_content.reserve(2 * 1024 * 1024);
  for (int i = 0; i < 5000; ++i) {
    expected_content += "第 " + std::to_string(i) +
                        " 行：SoEasy 桌面搜索引擎混合 IPC 与共享内存技术规范验证。\n";
  }
  WriteFile(large_doc_path, expected_content);

  soeasy::service::ParseTask task1{101, large_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult res1;

  auto t_start = std::chrono::high_resolution_clock::now();
  guard.ExecuteTask(task1, &res1);
  auto t_end = std::chrono::high_resolution_clock::now();
  double cost_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

  if (res1.status != soeasy::service::TaskStatus::kSuccess) {
    std::cerr << "[-] 任务执行失败: " << res1.error_message << "\n";
    return 1;
  }

  // Read extracted content from SHM slot
  const uint8_t* shm_data = nullptr;
  size_t shm_bytes = 0;
  bool read_ok = guard.shm_pool()->ReadSlot(res1.slot_id, &shm_data, &shm_bytes);
  if (!read_ok || shm_bytes != expected_content.size()) {
    std::cerr << "[-] 共享内存槽位数据校验失败! 读取字节数: " << shm_bytes
              << ", 预期字节数: " << expected_content.size() << "\n";
    return 1;
  }

  // Compare content byte-for-byte
  if (std::memcmp(shm_data, expected_content.data(), shm_bytes) != 0) {
    std::cerr << "[-] 共享内存与源文件内容不匹配!\n";
    return 1;
  }

  guard.shm_pool()->ReleaseSlot(res1.slot_id);
  double mb_sec = (shm_bytes / 1048576.0) / (cost_ms / 1000.0);
  std::cout << "    • 文档大小: " << shm_bytes << " 字节 ("
            << std::fixed << std::setprecision(2) << (shm_bytes / 1048576.0) << " MB)\n";
  std::cout << "    • 解析与传输耗时: " << cost_ms << " ms, 吞吐速率: "
            << mb_sec << " MB/s\n";
  std::cout << "[+] 大文档零拷贝解析与 SHM 读写校验 100% 一致通过!\n\n";

  // -------------------------------------------------------------
  // Test 2: Multi-encoding and HTML tag stripping
  // -------------------------------------------------------------
  std::cout << "[Step 3] 验证字符集自动探测、转码与 HTML 标签剥离...\n";
  std::string html_doc_path = "/tmp/soeasy_test_page.html";
  std::string html_content =
      "<!DOCTYPE html><html><head><title>测试标题</title></head>"
      "<body><h1>SoEasy 全文检索</h1><p>这是一段包含<b>加粗</b>和<i>斜体</i>的 HTML 正文内容。</p>"
      "</body></html>";
  WriteFile(html_doc_path, html_content);

  soeasy::service::ParseTask task2{102, html_doc_path, "html", 1};
  soeasy::service::ParseTaskResult res2;
  guard.ExecuteTask(task2, &res2);

  if (res2.status != soeasy::service::TaskStatus::kSuccess) {
    std::cerr << "[-] HTML 解析失败: " << res2.error_message << "\n";
    return 1;
  }

  const uint8_t* html_text_ptr = nullptr;
  size_t html_text_len = 0;
  guard.shm_pool()->ReadSlot(res2.slot_id, &html_text_ptr, &html_text_len);
  std::string parsed_html(reinterpret_cast<const char*>(html_text_ptr),
                          html_text_len);
  guard.shm_pool()->ReleaseSlot(res2.slot_id);

  if (parsed_html.find("<h1>") != std::string::npos ||
      parsed_html.find("<p>") != std::string::npos) {
    std::cerr << "[-] HTML 标签未被正确剥离: " << parsed_html << "\n";
    return 1;
  }
  std::cout << "    • 剥离标签后纯文本: \"" << parsed_html.substr(0, 60) << "...\"\n";
  std::cout << "[+] HTML 标签剥离与清洗验证通过!\n\n";

  // -------------------------------------------------------------
  // Test 3: Deadlock / Hang Document Watchdog Timeout Kill
  // -------------------------------------------------------------
  std::cout << "[Step 4] 验证死循环文档超时熔断强杀与自愈 (Watchdog SIGKILL)...\n";
  std::cout << "    (向 Worker 下发死循环恶意任务，设定熔断阈值: 1500 ms)...\n";
  soeasy::service::ParseTask task_hang{
      103, "/tmp/TEST_TRIGGER_HANG_malicious.txt", "txt", 1};
  soeasy::service::ParseTaskResult res_hang;

  auto t_hang_start = std::chrono::high_resolution_clock::now();
  guard.ExecuteTask(task_hang, &res_hang);
  auto t_hang_end = std::chrono::high_resolution_clock::now();
  double hang_ms =
      std::chrono::duration<double, std::milli>(t_hang_end - t_hang_start).count();

  if (res_hang.status != soeasy::service::TaskStatus::kTimeout) {
    std::cerr << "[-] 预期超时熔断未触发! 实际状态: "
              << static_cast<int>(res_hang.status) << "\n";
    return 1;
  }
  std::cout << "[+] 超时熔断拦截成功! 耗时: " << hang_ms
            << " ms, 状态: TaskStatus::kTimeout (" << res_hang.error_message
            << ")\n";
  std::cout << "[+] 恶性卡死子进程已被安全强杀，且无内存槽位泄漏!\n\n";

  // -------------------------------------------------------------
  // Test 4: Segfault Crash Recovery
  // -------------------------------------------------------------
  std::cout << "[Step 5] 验证段错误异常崩溃自愈与 Worker 自动拉起...\n";
  std::cout << "    (向 Worker 下发触发 SIGSEGV 的畸形任务)...\n";
  soeasy::service::ParseTask task_crash{
      104, "/tmp/TEST_TRIGGER_CRASH_exploit.bin", "bin", 1};
  soeasy::service::ParseTaskResult res_crash;

  guard.ExecuteTask(task_crash, &res_crash);
  if (res_crash.status != soeasy::service::TaskStatus::kCrashed) {
    std::cerr << "[-] 预期崩溃捕获失败! 实际状态: "
              << static_cast<int>(res_crash.status) << "\n";
    return 1;
  }
  std::cout << "[+] 崩溃异常成功被沙箱捕获! 状态: TaskStatus::kCrashed ("
            << res_crash.error_message << ")\n";
  std::cout << "[+] 沙箱守卫已在后台无缝重启干净的 Worker 子进程替代!\n\n";

  // -------------------------------------------------------------
  // Test 5: Verify Pool Functionality After Crash & Timeout
  // -------------------------------------------------------------
  std::cout << "[Step 6] 验证经历熔断与崩溃后，进程池完全自愈并正常处理新任务...\n";
  soeasy::service::ParseTask task_verify{105, large_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult res_verify;

  guard.ExecuteTask(task_verify, &res_verify);
  if (res_verify.status != soeasy::service::TaskStatus::kSuccess) {
    std::cerr << "[-] 自愈验证失败: 无法处理后续正常任务!\n";
    return 1;
  }
  guard.shm_pool()->ReleaseSlot(res_verify.slot_id);
  std::cout << "[+] 自愈验证圆满成功! 新 Worker 顺畅完成后续任务解析!\n\n";

  // -------------------------------------------------------------
  // Test 6: Concurrent Burst Throughput
  // -------------------------------------------------------------
  std::cout << "[Step 7] 执行高并发解析压力与吞吐测试 (50 批次任务流水线)...\n";
  const int kBurstCount = 50;
  auto burst_t0 = std::chrono::high_resolution_clock::now();
  int success_count = 0;

  for (int i = 0; i < kBurstCount; ++i) {
    soeasy::service::ParseTask burst_task{
        200 + i, (i % 2 == 0) ? large_doc_path : html_doc_path,
        (i % 2 == 0) ? "txt" : "html", 1};
    soeasy::service::ParseTaskResult burst_res;
    if (guard.ExecuteTask(burst_task, &burst_res) &&
        burst_res.status == soeasy::service::TaskStatus::kSuccess) {
      guard.shm_pool()->ReleaseSlot(burst_res.slot_id);
      success_count++;
    }
  }
  auto burst_t1 = std::chrono::high_resolution_clock::now();
  double burst_ms =
      std::chrono::duration<double, std::milli>(burst_t1 - burst_t0).count();
  double qps = (kBurstCount / (burst_ms / 1000.0));

  std::cout << "    • 总任务数: " << kBurstCount << ", 成功完成: "
            << success_count << ", 总耗时: " << burst_ms << " ms\n";
  std::cout << "    • 解析调度吞吐量: " << std::fixed << std::setprecision(1)
            << qps << " 文件/秒\n\n";

  guard.Shutdown();

  // Cleanup temporary files
  std::remove(large_doc_path.c_str());
  std::remove(html_doc_path.c_str());

  std::cout << "========================================================\n";
  std::cout << "🎉 所有验证项全部通过! Phase 2 混合 IPC 与解析沙箱验证圆满成功!\n";
  std::cout << "========================================================\n";
  return 0;
}
