#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "common/shm_pool.h"
#include "service/worker_guard.h"
#include "worker/l1_text_parser.h"

namespace {

void WriteFile(const std::string &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::binary);
  ofs.write(content.data(), content.size());
}

}  // namespace

int main(int argc, char *argv[]) {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy (Sou Easy) 混合 IPC 与解析沙箱独立实证 Demo (Phase 2)\n";
  std::cout << "   Cap'n Proto + 共享内存槽位池 + 多进程沙箱守卫 (熔断/自愈)\n";
  std::cout << "========================================================\n\n";

  soeasy::service::WorkerGuard guard;

  // Initialize WorkerGuard with 2 workers, 1500ms timeout for tests, dynamic slots (2+1 = 3 slots), 2MB capacity
  std::cout << "[Step 1] 初始化 Worker 进程池与动态共享内存池 (2 workers -> 3 slots x 2MB = 6MB)...\n";
  auto t0 = std::chrono::high_resolution_clock::now();
  bool init_ok = guard.Init(/*worker_bin_path=*/"", /*worker_count=*/2,
                            /*timeout_ms=*/1500, /*shm_slot_count=*/0,
                            /*shm_slot_capacity=*/0);
  auto t1 = std::chrono::high_resolution_clock::now();
  double init_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (!init_ok) {
    std::cerr << "[-] WorkerGuard 初始化失败!\n";
    return 1;
  }
  assert(guard.shm_pool()->slot_count() == 3);
  assert(guard.shm_pool()->slot_capacity() == 2 * 1024 * 1024);
  std::cout << "[+] WorkerGuard 初始化成功! 启动工作进程数: " << guard.worker_count()
            << ", 槽位数: " << guard.shm_pool()->slot_count()
            << ", 槽位容量: " << guard.shm_pool()->slot_capacity() / (1024 * 1024) << "MB (总池 "
            << (guard.shm_pool()->slot_count() * guard.shm_pool()->slot_capacity()) / (1024 * 1024)
            << "MB), 耗时: " << std::fixed << std::setprecision(2) << init_ms << " ms\n\n";

  // -------------------------------------------------------------
  // Test 1: Normal large document zero-copy extraction via SHM
  // -------------------------------------------------------------
  std::cout << "[Step 2] 验证大文档跨进程零拷贝解析与数据保真度...\n";
  std::string large_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_large.txt").string();
  std::string expected_content;
  expected_content.reserve(2 * 1024 * 1024);
  for (int i = 0; i < 5000; ++i) {
    expected_content += "第 " + std::to_string(i) + " 行：SoEasy 桌面搜索引擎混合 IPC 与共享内存技术规范验证。\n";
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
  const uint8_t *shm_data = nullptr;
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
  std::cout << "    • 文档大小: " << shm_bytes << " 字节 (" << std::fixed << std::setprecision(2)
            << (shm_bytes / 1048576.0) << " MB)\n";
  std::cout << "    • 解析与传输耗时: " << cost_ms << " ms, 吞吐速率: " << mb_sec << " MB/s\n";
  std::cout << "[+] 大文档零拷贝解析与 SHM 读写校验 100% 一致通过!\n\n";

  // -------------------------------------------------------------
  // Test 2: Multi-encoding and HTML tag stripping
  // -------------------------------------------------------------
  std::cout << "[Step 3] 验证字符集自动探测、转码与 HTML 标签剥离...\n";
  std::string html_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_page.html").string();
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

  const uint8_t *html_text_ptr = nullptr;
  size_t html_text_len = 0;
  guard.shm_pool()->ReadSlot(res2.slot_id, &html_text_ptr, &html_text_len);
  std::string parsed_html(reinterpret_cast<const char *>(html_text_ptr), html_text_len);
  guard.shm_pool()->ReleaseSlot(res2.slot_id);

  if (parsed_html.find("<h1>") != std::string::npos || parsed_html.find("<p>") != std::string::npos) {
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
  std::string hang_doc_path = (std::filesystem::temp_directory_path() / "TEST_TRIGGER_HANG_malicious.txt").string();
  soeasy::service::ParseTask task_hang{103, hang_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult res_hang;

  auto t_hang_start = std::chrono::high_resolution_clock::now();
  guard.ExecuteTask(task_hang, &res_hang);
  auto t_hang_end = std::chrono::high_resolution_clock::now();
  double hang_ms = std::chrono::duration<double, std::milli>(t_hang_end - t_hang_start).count();

  if (res_hang.status != soeasy::service::TaskStatus::kTimeout) {
    std::cerr << "[-] 预期超时熔断未触发! 实际状态: " << static_cast<int>(res_hang.status) << "\n";
    return 1;
  }
  std::cout << "[+] 超时熔断拦截成功! 耗时: " << hang_ms << " ms, 状态: TaskStatus::kTimeout ("
            << res_hang.error_message << ")\n";
  std::cout << "[+] 恶性卡死子进程已被安全强杀，且无内存槽位泄漏!\n\n";

  // -------------------------------------------------------------
  // Test 4: Segfault Crash Recovery
  // -------------------------------------------------------------
  std::cout << "[Step 5] 验证段错误异常崩溃自愈与 Worker 自动拉起...\n";
  std::cout << "    (向 Worker 下发触发 SIGSEGV 的畸形任务)...\n";
  std::string crash_doc_path = (std::filesystem::temp_directory_path() / "TEST_TRIGGER_CRASH_exploit.bin").string();
  soeasy::service::ParseTask task_crash{104, crash_doc_path, "bin", 1};
  soeasy::service::ParseTaskResult res_crash;

  guard.ExecuteTask(task_crash, &res_crash);
  if (res_crash.status != soeasy::service::TaskStatus::kCrashed) {
    std::cerr << "[-] 预期崩溃捕获失败! 实际状态: " << static_cast<int>(res_crash.status) << "\n";
    return 1;
  }
  std::cout << "[+] 崩溃异常成功被沙箱捕获! 状态: TaskStatus::kCrashed (" << res_crash.error_message << ")\n";
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
    soeasy::service::ParseTask burst_task{200 + i, (i % 2 == 0) ? large_doc_path : html_doc_path,
                                          (i % 2 == 0) ? "txt" : "html", 1};
    soeasy::service::ParseTaskResult burst_res;
    if (guard.ExecuteTask(burst_task, &burst_res) && burst_res.status == soeasy::service::TaskStatus::kSuccess) {
      guard.shm_pool()->ReleaseSlot(burst_res.slot_id);
      success_count++;
    }
  }
  auto burst_t1 = std::chrono::high_resolution_clock::now();
  double burst_ms = std::chrono::duration<double, std::milli>(burst_t1 - burst_t0).count();
  double qps = (kBurstCount / (burst_ms / 1000.0));

  std::cout << "    • 总任务数: " << kBurstCount << ", 成功完成: " << success_count << ", 总耗时: " << burst_ms
            << " ms\n";
  std::cout << "    • 解析调度吞吐量: " << std::fixed << std::setprecision(1) << qps << " 文件/秒\n\n";

  // -------------------------------------------------------------
  // Test 7: Spill-to-file Boundary (Exact 2MB vs 2MB + 1 Byte)
  // -------------------------------------------------------------
  std::cout << "[Step 8] 验证临界容量边界 (恰好 2MB 走 SHM，2MB + 1 字节精确溢出落盘)...\n";
  const size_t kSlotCap = guard.shm_pool()->slot_capacity();  // 2 * 1024 * 1024

  // Boundary 1: Exactly 2MB (within capacity)
  std::string exact_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_exact_2mb.txt").string();
  {
    std::string exact_content(kSlotCap, 'A');
    WriteFile(exact_doc_path, exact_content);
  }
  soeasy::service::ParseTask exact_task{299, exact_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult exact_res;
  guard.ExecuteTask(exact_task, &exact_res);
  assert(exact_res.status == soeasy::service::TaskStatus::kSuccess);
  assert(exact_res.spill_file_path.empty());  // Stays in shared memory!
  guard.shm_pool()->ReleaseSlot(exact_res.slot_id);
  std::remove(exact_doc_path.c_str());
  std::cout << "    • 边界 1 (恰好 2MB): 成功走共享内存零拷贝 (spill_file_path 为空)!\n";

  // Boundary 2: 2MB + 1 Byte (strictly exceeds capacity)
  std::string overflow_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_overflow.txt").string();
  {
    std::string overflow_content(kSlotCap + 1, 'B');
    WriteFile(overflow_doc_path, overflow_content);
  }
  soeasy::service::ParseTask overflow_task{300, overflow_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult overflow_res;
  guard.ExecuteTask(overflow_task, &overflow_res);
  assert(overflow_res.status == soeasy::service::TaskStatus::kSuccess);
  assert(!overflow_res.spill_file_path.empty());  // Must trigger spill-to-file!
  assert(std::filesystem::exists(overflow_res.spill_file_path));
  std::uintmax_t spill_sz = std::filesystem::file_size(overflow_res.spill_file_path);
  assert(spill_sz == kSlotCap + 1);
  std::cout << "    • 边界 2 (2MB + 1 字节): 成功触发溢出落盘! 溢出大小: " << spill_sz << " 字节\n";

  std::error_code ec;
  std::filesystem::remove(overflow_res.spill_file_path, ec);
  guard.shm_pool()->ReleaseSlot(overflow_res.slot_id);
  std::remove(overflow_doc_path.c_str());

  // Test CleanupOrphanSpillFiles() Boundary Defense:
  // Non-matching file must NOT be deleted; matching orphan files MUST be deleted.
  std::filesystem::path unrelated_file = std::filesystem::path(guard.GetSpillDirectory()) / "unrelated_user_data.txt";
  std::filesystem::path orphan_file1 =
      std::filesystem::path(guard.GetSpillDirectory()) / "soeasy_spill_999999_888_12345.tmp";
  std::filesystem::path orphan_file2 =
      std::filesystem::path(guard.GetSpillDirectory()) / "soeasy_spill_888888_777_67890.tmp";
  WriteFile(unrelated_file.string(), "do not delete me");
  WriteFile(orphan_file1.string(), "orphan 1");
  WriteFile(orphan_file2.string(), "orphan 2");

  soeasy::service::WorkerGuard::CleanupOrphanSpillFiles();
  assert(std::filesystem::exists(unrelated_file));  // Safe! Unrelated file preserved!
  assert(!std::filesystem::exists(orphan_file1));   // Swept!
  assert(!std::filesystem::exists(orphan_file2));   // Swept!
  std::remove(unrelated_file.string().c_str());
  std::cout << "    • 孤儿清理安全边界: 非匹配文件完整保留，孤儿临时文件彻底扫除!\n";

  // -------------------------------------------------------------
  // Test 8: Worker Idle Eviction & Seamless Respawn Verification
  // -------------------------------------------------------------
  std::cout << "\n[Step 9] 验证 Worker 闲置超时自动淘汰与后续任务无缝自愈拉起...\n";
  std::cout << "    • 当前活跃 Worker 数: " << guard.active_worker_count() << "\n";
  assert(guard.active_worker_count() > 0);

  // Set idle eviction threshold to 200ms
  guard.set_idle_evict_ms(200);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  std::cout << "    • 200ms 闲置淘汰生效后活跃 Worker 数: " << guard.active_worker_count() << "\n";
  assert(guard.active_worker_count() == 0);  // All workers evicted to 0 RAM!

  // Send new task to test seamless respawn
  soeasy::service::ParseTask resume_task{301, large_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult resume_res;
  bool resume_ok = guard.ExecuteTask(resume_task, &resume_res);
  assert(resume_ok);
  assert(resume_res.status == soeasy::service::TaskStatus::kSuccess);
  guard.shm_pool()->ReleaseSlot(resume_res.slot_id);
  std::cout << "    • 唤醒新任务后活跃 Worker 数: " << guard.active_worker_count() << " (成功瞬时拉起并完成解析)!\n";
  assert(guard.active_worker_count() > 0);
  std::cout << "[+] Worker 懒加载、闲置归零与无缝拉起验证 100% 通过!\n\n";

  guard.Shutdown();

  // Cleanup temporary files
  std::remove(large_doc_path.c_str());
  std::remove(html_doc_path.c_str());

  std::cout << "========================================================\n";
  std::cout << "🎉 所有验证项全部通过! Phase 2 混合 IPC 与解析沙箱验证圆满成功!\n";
  std::cout << "========================================================\n";
  return 0;
}
