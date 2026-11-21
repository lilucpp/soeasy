#include <atomic>
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

#include "service/worker_guard.h"
#include "test_helper.h"
#include "worker/l1_text_parser.h"

namespace {

void WriteFile(const std::string &path, const std::string &content) {
  std::ofstream ofs(path, std::ios::binary);
  ofs.write(content.data(), content.size());
}

}  // namespace

int main(int argc, char *argv[]) {
  if (argc >= 2 && std::strcmp(argv[1], "--child-noisy-stdio") == 0) {
    // Emit >512 KB to both stdout and stderr to exceed Windows (4 KB) and POSIX (64 KB) pipe buffers.
    const std::string line = "Using ODF/OOXML parser. Regression check line for pipe deadlock.\n";
    for (int i = 0; i < 8192; ++i) {
      std::cout << line;
      std::cerr << line;
    }
    std::cout.flush();
    std::cerr.flush();
    return 0;
  }

  std::cout << "========================================================\n";
  std::cout << "   SoEasy (Sou Easy) Cap'n Proto RPC 沙箱独立实证 Demo\n";
  std::cout << "   Cap'n Proto RPC + 多进程沙箱守卫 (熔断/自愈/流式)\n";
  std::cout << "========================================================\n\n";

  soeasy::service::WorkerGuard guard;

  // Initialize WorkerGuard with 2 workers, 1500ms timeout for tests
  std::cout << "[Step 1] 初始化 Worker 进程池 (2 workers, 1500ms timeout)...\n";
  auto t0 = std::chrono::high_resolution_clock::now();
  std::string worker_bin = soeasy::test::FindWorkerBin();
  bool init_ok = guard.Init(worker_bin, /*worker_count=*/2, /*timeout_ms=*/1500);
  auto t1 = std::chrono::high_resolution_clock::now();
  double init_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (!init_ok) {
    std::cerr << "[-] WorkerGuard 初始化失败!\n";
    return 1;
  }
  assert(guard.worker_count() == 2);
  std::cout << "[+] WorkerGuard 初始化成功! 启动工作进程数: " << guard.worker_count()
            << ", 耗时: " << std::fixed << std::setprecision(2) << init_ms << " ms\n\n";

  // -------------------------------------------------------------
  // Test 1: Normal large document extraction via Cap'n Proto RPC
  // -------------------------------------------------------------
  std::cout << "[Step 2] 验证大文档跨进程 Cap'n Proto RPC 解析与数据保真度...\n";
  std::string large_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_large.txt").string();
  std::string expected_content;
  expected_content.reserve(2 * 1024 * 1024);
  for (int i = 0; i < 5000; ++i) {
    expected_content += "第 " + std::to_string(i) + " 行：SoEasy 桌面搜索引擎 Cap'n Proto RPC 传输验证。\n";
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

  // Compare content byte-for-byte directly from RPC
  if (res1.extracted_text != expected_content) {
    std::cerr << "[-] RPC 传输内容与源文件内容不匹配!\n";
    return 1;
  }

  double mb_sec = (res1.extracted_text.size() / 1048576.0) / (cost_ms / 1000.0);
  std::cout << "    • 文档大小: " << res1.extracted_text.size() << " 字节 (" << std::fixed << std::setprecision(2)
            << (res1.extracted_text.size() / 1048576.0) << " MB)\n";
  std::cout << "    • 解析与传输耗时: " << cost_ms << " ms, 吞吐速率: " << mb_sec << " MB/s\n";
  std::cout << "[+] 大文档 Cap'n Proto RPC 解析与读写校验 100% 一致通过!\n\n";

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

  std::string parsed_html = res2.extracted_text;
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
  soeasy::service::ParseTask task_hang{103, hang_doc_path, "txt", 1, 1500};
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
  std::cout << "[+] 恶性卡死子进程已被安全强杀并隔离!\n\n";

  // -------------------------------------------------------------
  // Test 3b: Watchdog Stall Kill
  // -------------------------------------------------------------
  std::cout << "[Step 4b] 验证 Watchdog 超时熔断拦截 (2000ms timeout) 状态保真...\n";
  soeasy::service::ParseTask task_stall{1030, hang_doc_path, "txt", 1, 2000};
  soeasy::service::ParseTaskResult res_stall;
  guard.ExecuteTask(task_stall, &res_stall);
  if (res_stall.status != soeasy::service::TaskStatus::kTimeout) {
    std::cerr << "[-] Watchdog 超时状态误判! 期望 kTimeout(3), 实际状态: " << static_cast<int>(res_stall.status)
              << " (" << res_stall.error_message << ")\n";
    return 1;
  }
  std::cout << "[+] Watchdog 超时熔断正确归类为 TaskStatus::kTimeout (" << res_stall.error_message << ")\n\n";

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
  // Test 7: Zero-Spill Streaming Boundary (Exact 2MB vs 2MB + 1 Byte)
  // -------------------------------------------------------------
  std::cout << "[Step 8] 验证临界容量边界 (恰好 2MB 与 2MB + 1 字节流式分片传输)...\n";
  const size_t kSlotCap = 2 * 1024 * 1024;

  // Boundary 1: Exactly 2MB
  std::string exact_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_exact_2mb.txt").string();
  {
    std::string exact_content(kSlotCap, 'A');
    WriteFile(exact_doc_path, exact_content);
  }
  soeasy::service::ParseTask exact_task{299, exact_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult exact_res;
  guard.ExecuteTask(exact_task, &exact_res);
  assert(exact_res.status == soeasy::service::TaskStatus::kSuccess);
  assert(exact_res.extracted_text.size() == kSlotCap);
  std::remove(exact_doc_path.c_str());
  std::cout << "    • 边界 1 (恰好 2MB): 成功获取完整文本 (大小: " << exact_res.extracted_text.size() << " 字节)!\n";

  // Boundary 2: 2MB + 1 Byte
  std::string overflow_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_overflow.txt").string();
  {
    std::string overflow_content(kSlotCap + 1, 'B');
    WriteFile(overflow_doc_path, overflow_content);
  }
  soeasy::service::ParseTask overflow_task{300, overflow_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult overflow_res;
  guard.ExecuteTask(overflow_task, &overflow_res);
  assert(overflow_res.status == soeasy::service::TaskStatus::kSuccess);
  assert(overflow_res.extracted_text.size() == kSlotCap + 1);
  assert(overflow_res.stream_chunk_count > 0);
  std::cout << "    • 边界 2 (2MB + 1 字节): 成功触发流式分片，零磁盘溢出! 接收大小: "
            << overflow_res.extracted_text.size() << " 字节，分片数: " << overflow_res.stream_chunk_count << "\n";
  std::remove(overflow_doc_path.c_str());

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
  assert(guard.active_worker_count() == 0);      // All workers evicted to 0 RAM!

  // Send new task to test seamless respawn
  soeasy::service::ParseTask resume_task{301, large_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult resume_res;
  bool resume_ok = guard.ExecuteTask(resume_task, &resume_res);
  assert(resume_ok);
  (void)resume_ok;
  assert(resume_res.status == soeasy::service::TaskStatus::kSuccess);
  std::cout << "    • 唤醒新任务后活跃 Worker 数: " << guard.active_worker_count()
            << " (成功瞬时拉起)!\n";
  assert(guard.active_worker_count() > 0);
  std::cout << "[+] Worker 懒加载、闲置归零与无缝拉起验证 100% 通过!\n\n";

  guard.Shutdown();

  // -------------------------------------------------------------
  // Test 9: Combined stdout/stderr pipe drain (>512KB non-blocking)
  // -------------------------------------------------------------
  std::cout << "[Step 10] 验证 subprocess_option_combined_stdout_stderr 持续排空防管道写满死锁 (>512KB)...\n";
  {
    const char *noisy_cmd[] = {argv[0], "--child-noisy-stdio", nullptr};
    subprocess_s noisy_proc{};
    int spawn_ret = subprocess_create(
        noisy_cmd, subprocess_option_inherit_environment | subprocess_option_combined_stdout_stderr, &noisy_proc);
    if (spawn_ret != 0 || subprocess_stdout(&noisy_proc) == nullptr) {
      std::cerr << "[-] subprocess_create(combined_stdout_stderr) failed: " << spawn_ret << "\n";
      return 1;
    }
    size_t drained_bytes = 0;
    std::thread drain_thread([&]() {
      char buf[4096];
      while (size_t n = std::fread(buf, 1, sizeof(buf), subprocess_stdout(&noisy_proc))) {
        drained_bytes += n;
      }
    });
    int noisy_exit = -1;
    int join_ret = subprocess_join(&noisy_proc, &noisy_exit);
    drain_thread.join();
    if (join_ret != 0 || noisy_exit != 0 || drained_bytes < 512 * 1024) {
      std::cerr << "[-] noisy stdio child failed: join_ret=" << join_ret << " exit=" << noisy_exit
                << " drained=" << drained_bytes << "\n";
      subprocess_destroy(&noisy_proc);
      return 1;
    }
    subprocess_destroy(&noisy_proc);
    std::cout << "[+] 持续排空验证通过: 成功无阻塞读取 " << drained_bytes << " 字节并正常退出!\n\n";
  }

  // -------------------------------------------------------------
  // Test 10: High-Concurrency Crash Storm (8 threads x 4 workers)
  // -------------------------------------------------------------
  std::cout << "[Step 11] 验证高并发崩溃风暴下多 Worker 状态稳定隔离 (8 线程 x 4 Worker)...\n";
  {
    const std::filesystem::path storm_dir = std::filesystem::temp_directory_path() / "soeasy_crash_storm_test";
    std::error_code ec_storm;
    std::filesystem::remove_all(storm_dir, ec_storm);
    std::filesystem::create_directories(storm_dir / "logs", ec_storm);

    soeasy::service::WorkerGuard storm_guard;
    storm_guard.SetLoggingOptions("debug", false, true, false, "", false, (storm_dir / "logs").string());
    if (!storm_guard.Init(worker_bin, /*worker_count=*/4, /*timeout_ms=*/8000)) {
      std::cerr << "[-] storm_guard Init failed!\n";
      return 1;
    }

    constexpr int kThreads = 8;
    constexpr int kItersPerThread = 12;
    std::atomic<int> normal_ok{0};
    std::atomic<int> crash_ok{0};
    std::atomic<int> anomalies{0};
    std::vector<std::thread> workers_threads;
    workers_threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
      workers_threads.emplace_back([&, t]() {
        for (int k = 0; k < kItersPerThread; ++k) {
          if (k % 3 == 0) {
            soeasy::service::ParseTask ctask{10000 + t * 100 + k, crash_doc_path, "bin", 1};
            soeasy::service::ParseTaskResult cres;
            if (!storm_guard.ExecuteTask(ctask, &cres) || cres.status != soeasy::service::TaskStatus::kCrashed) {
              std::cerr << "[-] Crash task unexpected status=" << static_cast<int>(cres.status)
                        << " err=" << cres.error_message << "\n";
              anomalies.fetch_add(1, std::memory_order_relaxed);
            } else {
              crash_ok.fetch_add(1, std::memory_order_relaxed);
            }
          } else {
            const std::string token = "StormToken_T" + std::to_string(t) + "_K" + std::to_string(k);
            const std::string p =
                (storm_dir / ("doc_" + std::to_string(t) + "_" + std::to_string(k) + ".txt")).string();
            WriteFile(p, token);
            soeasy::service::ParseTask ntask{20000 + t * 100 + k, p, "txt", 1};
            soeasy::service::ParseTaskResult nres;
            if (!storm_guard.ExecuteTask(ntask, &nres) || nres.status != soeasy::service::TaskStatus::kSuccess) {
              std::cerr << "[-] Normal task failed during crash storm: status=" << static_cast<int>(nres.status)
                        << " err=" << nres.error_message << "\n";
              anomalies.fetch_add(1, std::memory_order_relaxed);
            } else {
              std::string extracted = nres.extracted_text;
              if (extracted.find(token) == std::string::npos) {
                std::cerr << "[-] RPC payload mismatch! expected token=" << token << "\n";
                anomalies.fetch_add(1, std::memory_order_relaxed);
              } else {
                normal_ok.fetch_add(1, std::memory_order_relaxed);
              }
            }
            std::remove(p.c_str());
          }
        }
      });
    }

    for (auto &th : workers_threads) {
      th.join();
    }

    storm_guard.Shutdown();
    std::filesystem::remove_all(storm_dir, ec_storm);

    if (anomalies.load() != 0 || crash_ok.load() != kThreads * (kItersPerThread / 3) ||
        normal_ok.load() != kThreads * (kItersPerThread - kItersPerThread / 3)) {
      std::cerr << "[-] Crash storm check failed: anomalies=" << anomalies.load() << " crash_ok=" << crash_ok.load()
                << " normal_ok=" << normal_ok.load() << "\n";
      return 1;
    }
    std::cout << "[+] 8 线程 x 4 Worker 高并发崩溃风暴通过: crash_ok=" << crash_ok.load()
              << ", normal_ok=" << normal_ok.load() << ", 0 误杀/0 串包!\n\n";
  }

  // -------------------------------------------------------------
  // Test 11: Cap'n Proto Chunked Streaming Verification
  // -------------------------------------------------------------
  std::cout << "[Step 12] 验证 Cap'n Proto 跨进程流式分片传输 (256KB Chunk Streaming)...\n";
  std::string stream_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_stream_chunk.txt").string();
  std::string stream_content;
  stream_content.reserve(700 * 1024);
  for (int i = 0; i < 15000; ++i) {
    stream_content += "Cap'n Proto chunk stream verification line " + std::to_string(i) + "\n";
  }
  WriteFile(stream_doc_path, stream_content);

  soeasy::service::WorkerGuard stream_guard;
  stream_guard.Init(worker_bin, 1, 3000);

  soeasy::service::ParseTask stream_task{9901, stream_doc_path, "txt", 1};
  soeasy::service::ParseTaskResult stream_res;
  stream_guard.ExecuteTask(stream_task, &stream_res);
  assert(stream_res.status == soeasy::service::TaskStatus::kSuccess);
  assert(stream_res.extracted_text == stream_content);
  assert(stream_res.stream_chunk_count > 1);
  stream_guard.Shutdown();
  std::remove(stream_doc_path.c_str());
  std::cout << "    • 文档大小: " << stream_content.size() << " 字节, 接收分片数: " << stream_res.stream_chunk_count
            << ", 拼装后 100% 一致!\n";
  std::cout << "[+] Cap'n Proto 流式分片传输验证成功!\n\n";

  // -------------------------------------------------------------
  // Test 12: Task-Boundary Memory Recycling Verification
  // -------------------------------------------------------------
  std::cout << "[Step 13] 验证 Worker 任务完成点内存水位检测与持续高压下即时回收...\n";
  {
    const std::string mem_doc_path = (std::filesystem::temp_directory_path() / "soeasy_test_mem_recycle.txt").string();
    const std::string mem_token = "Step13MemoryRecycleVerificationToken";
    WriteFile(mem_doc_path, mem_token + "\n");

    soeasy::service::WorkerGuard mem_guard;
    if (!mem_guard.Init(worker_bin, /*worker_count=*/1, /*timeout_ms=*/5000)) {
      std::cerr << "[-] mem_guard Init failed!\n";
      std::remove(mem_doc_path.c_str());
      return 1;
    }

    // Case A: Back-to-back tasks under continuous load with tight threshold (1 MiB) and interval=1.
    mem_guard.set_idle_evict_ms(60000);
    mem_guard.set_max_memory_bytes(1 * 1024 * 1024);
    mem_guard.set_memory_check_interval_tasks(1);

    for (uint64_t i = 1; i <= 3; ++i) {
      soeasy::service::ParseTask mtask{9950 + static_cast<int64_t>(i), mem_doc_path, "txt", 1};
      soeasy::service::ParseTaskResult mres;
      bool ok = mem_guard.ExecuteTask(mtask, &mres);
      if (!ok || mres.status != soeasy::service::TaskStatus::kSuccess ||
          mres.extracted_text.find(mem_token) == std::string::npos || mem_guard.memory_recycle_count() != i ||
          mem_guard.active_worker_count() != 0) {
        std::cerr << "[-] Case A failed at iter " << i << ": ok=" << ok
                  << " status=" << static_cast<int>(mres.status)
                  << " recycles=" << mem_guard.memory_recycle_count()
                  << " active=" << mem_guard.active_worker_count() << "\n";
        mem_guard.Shutdown();
        std::remove(mem_doc_path.c_str());
        return 1;
      }
    }
    std::cout << "    • Case A (高压连续任务阈值触发): 3 次连续执行均在任务交接点即时回收, recycle_count="
              << mem_guard.memory_recycle_count() << "\n";

    // Case B: Healthy worker under default 256 MiB threshold is reused without spurious recycling.
    mem_guard.set_max_memory_bytes(soeasy::service::WorkerGuard::kDefaultMaxMemoryBytes);
    mem_guard.set_memory_check_interval_tasks(2);
    const uint64_t before_recycles = mem_guard.memory_recycle_count();
    for (int i = 0; i < 4; ++i) {
      soeasy::service::ParseTask mtask{9960 + i, mem_doc_path, "txt", 1};
      soeasy::service::ParseTaskResult mres;
      bool ok = mem_guard.ExecuteTask(mtask, &mres);
      if (!ok || mres.status != soeasy::service::TaskStatus::kSuccess ||
          mres.extracted_text.find(mem_token) == std::string::npos) {
        std::cerr << "[-] Case B task failed at iter " << i << "\n";
        mem_guard.Shutdown();
        std::remove(mem_doc_path.c_str());
        return 1;
      }
    }
    if (mem_guard.memory_recycle_count() != before_recycles || mem_guard.active_worker_count() != 1) {
      std::cerr << "[-] Case B spurious recycle check failed: before=" << before_recycles
                << " after=" << mem_guard.memory_recycle_count()
                << " active=" << mem_guard.active_worker_count() << "\n";
      mem_guard.Shutdown();
      std::remove(mem_doc_path.c_str());
      return 1;
    }
    std::cout << "    • Case B (正常 256 MiB 水位复用): 4 次连续执行无误回收, active_workers="
              << mem_guard.active_worker_count() << "\n";

    mem_guard.Shutdown();
    std::remove(mem_doc_path.c_str());
    std::cout << "[+] Worker 任务完成点水位回收与安全复用验证通过!\n\n";
  }

  // Cleanup temporary files
  std::remove(large_doc_path.c_str());
  std::remove(html_doc_path.c_str());

  std::cout << "========================================================\n";
  std::cout << "🎉 所有验证项全部通过! Cap'n Proto RPC 沙箱实证圆满成功!\n";
  std::cout << "========================================================\n";
  return 0;
}
