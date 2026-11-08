// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "sqlite3mc_amalgamation.h"
#include "storage/istorage_engine.h"
#include "storage/reader_connection_pool.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

int main() {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy Reader Connection Pool & WAL Dual-Channel Test\n";
  std::cout << "========================================================\n\n";

  const std::string db_path = soeasy::test::GetTestTempPath("soeasy_test_reader_pool.db").string();
  const std::string key = "Pool_Secret_Key_2026!";
  const std::string dict_dir = soeasy::test::FindDictDir();

  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());

  // -------------------------------------------------------------
  // Test 1: Initialize Database via StorageEngine & verify schema
  // -------------------------------------------------------------
  std::cout << "[Test 1] Initializing StorageEngine with DB: " << db_path << "\n";
  soeasy::storage::StorageEngine engine;
  if (!engine.Open(db_path, key, dict_dir)) {
    std::cerr << "[-] Failed to open StorageEngine: " << engine.GetLastError() << "\n";
    return 1;
  }

  // Insert initial seed data
  for (int i = 1; i <= 20; ++i) {
    std::string title = "文档_" + std::to_string(i) + ".txt";
    std::string path = "/test/path/doc_" + std::to_string(i) + ".txt";
    std::string content = "这是关于人工智能和向量数据库的第 " + std::to_string(i) + " 篇技术架构分析文档。";
    if (!engine.IndexDocument(i, path, title, ".txt", 1024, 1600000000 + i, 1, content)) {
      std::cerr << "[-] Failed to insert seed doc " << i << "\n";
      return 1;
    }
  }
  std::cout << "[+] Successfully inserted 20 seed documents.\n";

  // -------------------------------------------------------------
  // Test 2: ReaderConnectionPool Lifecycle, Lease & Timeout
  // -------------------------------------------------------------
  std::cout << "\n[Test 2] Direct ReaderConnectionPool Lease & Timeout Verification\n";
  {
    soeasy::storage::ReaderConnectionPool pool;
    if (!pool.Init(db_path, key, dict_dir, 3)) {
      std::cerr << "[-] Failed to init ReaderConnectionPool: " << pool.GetLastError() << "\n";
      return 1;
    }

    assert(pool.capacity() == 3);
    assert(pool.available_count() == 3);

    // Lease conn 1
    auto conn1 = pool.Acquire(500);
    assert(conn1);
    assert(pool.available_count() == 2);

    // Lease conn 2 & 3
    auto conn2 = pool.Acquire(500);
    auto conn3 = pool.Acquire(500);
    assert(conn2 && conn3);
    assert(pool.available_count() == 0);

    // Lease conn 4: Pool is exhausted, should timeout
    auto t_start = std::chrono::steady_clock::now();
    auto conn4 = pool.Acquire(150);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t_start)
                          .count();
    assert(!conn4);
    assert(elapsed_ms >= 50);
    std::cout << "[+] Timeout lease correctly returned empty handle (waited " << elapsed_ms << "ms).\n";

    // Release conn1 by resetting scope
    {
      auto moved_conn = std::move(conn1);
      assert(!conn1);
    }  // moved_conn destructed and returned to pool
    assert(pool.available_count() == 1);

    // Now acquire should succeed immediately
    auto conn_reacquired = pool.Acquire(100);
    assert(conn_reacquired);
    assert(pool.available_count() == 0);

    pool.Close();
    std::cout << "[+] ReaderConnectionPool lease, return and close verified.\n";
  }

  // -------------------------------------------------------------
  // Test 3: Dual-Channel Concurrency: Long Writer vs. Concurrent Readers
  // -------------------------------------------------------------
  std::cout << "\n[Test 3] Dual-Channel Concurrency: 1 Writer holding long tx vs. 4 Readers\n";

  std::atomic<bool> writer_in_transaction{false};
  std::atomic<bool> stop_flag{false};
  std::atomic<uint64_t> total_reads{0};
  std::atomic<uint64_t> total_read_errors{0};

  // Launch 4 concurrent reader threads querying the storage engine
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&engine, &stop_flag, &total_reads, &total_read_errors, r]() {
      while (!stop_flag.load(std::memory_order_relaxed)) {
        auto results = engine.Search("人工智能", 10);
        if (!results.empty()) {
          total_reads.fetch_add(1, std::memory_order_relaxed);
          // Also test reading document content
          std::string content;
          if (!engine.GetDocumentContent(results[0].doc_id, &content)) {
            total_read_errors.fetch_add(1, std::memory_order_relaxed);
          }
        } else {
          total_read_errors.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });
  }

  // Writer thread executes a batch transaction with sleeps while holding transaction
  std::thread writer([&engine, &writer_in_transaction]() {
    std::cout << "[Writer] Starting batch transaction...\n";
    if (!engine.BeginTransaction()) {
      std::cerr << "[-] Failed to begin transaction\n";
      return;
    }
    writer_in_transaction.store(true);

    for (int i = 100; i < 150; ++i) {
      std::string path = "/test/path/batch_" + std::to_string(i) + ".txt";
      std::string title = "并发批量测试文档_" + std::to_string(i);
      std::string content = "这是在写事务过程中插入的高级并发内容 " + std::to_string(i);
      engine.IndexDocument(i, path, title, ".txt", 2048, 1700000000 + i, 1, content);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!engine.CommitTransaction()) {
      std::cerr << "[-] Failed to commit transaction\n";
    }
    writer_in_transaction.store(false);
    std::cout << "[Writer] Batch transaction committed successfully.\n";
  });

  // Let readers and writer run concurrently
  writer.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop_flag.store(true);

  for (auto &t : readers) {
    t.join();
  }

  std::cout << "[+] Reader results under active write transaction:\n";
  std::cout << "    Total reads completed: " << total_reads.load() << "\n";
  std::cout << "    Total read errors:     " << total_read_errors.load() << "\n";
  assert(total_reads.load() > 0);
  assert(total_read_errors.load() == 0);

  // -------------------------------------------------------------
  // Test 4: Post-Commit Visibility & Connection Pool Cleanup
  // -------------------------------------------------------------
  std::cout << "\n[Test 4] Post-Commit Visibility Check\n";
  auto post_results = engine.Search("并发批量", 100);
  assert(post_results.size() == 50);
  std::cout << "[+] Newly committed 50 records immediately visible to readers: found "
            << post_results.size() << " records.\n";

  engine.Close();
  std::cout << "[+] StorageEngine closed cleanly.\n";

  // Clean up test database
  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());

  std::cout << "\n[ALL CHECKS PASSED] Stage 2 Reader Connection Pool Verified Successfully!\n";
  return 0;
}
