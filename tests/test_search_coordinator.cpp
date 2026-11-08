// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <cassert>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "service/isearch_coordinator.h"
#include "service/name_index.h"
#include "service/search_coordinator.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

int main() {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy SearchCoordinator CQRS Query-Side Unit Test\n";
  std::cout << "========================================================\n\n";

  const std::string db_path = "/tmp/soeasy_test_search_coordinator.db";
  const std::string key = "Coordinator_Secret_2026!";
  const std::string dict_dir = soeasy::test::FindDictDir();

  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());

  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  if (!storage->Open(db_path, key, dict_dir)) {
    std::cerr << "[-] Failed to open StorageEngine: " << storage->GetLastError() << "\n";
    return 1;
  }

  auto name_index = std::make_shared<soeasy::service::NameIndex>();

  // 1. Seed NameIndex with sample files
  soeasy::service::NameRecord r1{101, "机器学习实战指南.pdf", "/data/docs/机器学习实战指南.pdf", ".pdf", 2048, 1600000001};
  soeasy::service::NameRecord r2{102, "人工智能系统架构.docx", "/data/docs/人工智能系统架构.docx", ".docx", 4096, 1600000002};
  soeasy::service::NameRecord r3{103, "数据库索引优化.txt", "/data/docs/数据库索引优化.txt", ".txt", 1024, 1600000003};
  name_index->Upsert(r1);
  name_index->Upsert(r2);
  name_index->Upsert(r3);

  // 2. Seed StorageEngine with content
  storage->IndexDocument(101, r1.file_path, r1.file_name, r1.ext, r1.file_size, r1.mtime, 1,
                         "本书讲解了机器学习中深度神经网络与模型微调的核心算法。");
  storage->IndexDocument(102, r2.file_path, r2.file_name, r2.ext, r2.file_size, r2.mtime, 1,
                         "本文档详细阐述了高可用分布式人工智能系统的设计与落地规范。");
  storage->IndexDocument(103, r3.file_path, r3.file_name, r3.ext, r3.file_size, r3.mtime, 1,
                         "深入浅出分析 SQLite B-Tree 与 WAL 机制在大规模向量检索中的应用。");

  soeasy::service::SearchCoordinator coordinator(name_index, storage);

  // -------------------------------------------------------------
  // Test 1: Filename Fast Substring & Prefix Search
  // -------------------------------------------------------------
  std::cout << "[Test 1] Filename Search via SearchCoordinator\n";
  int total_count = 0;
  auto name_hits = coordinator.SearchFilename("机器学习", 10, 0, "", &total_count);
  assert(total_count == 1);
  assert(name_hits.size() == 1);
  assert(name_hits[0].doc_id == 101);
  std::cout << "[+] Found filename: " << name_hits[0].file_name << "\n";

  // Substring match check
  auto sub_hits = coordinator.SearchFilename("实战指南", 10, 0, "", &total_count);
  assert(sub_hits.size() == 1);
  assert(sub_hits[0].doc_id == 101);
  std::cout << "[+] Substring match '实战指南' matched: " << sub_hits[0].file_name << "\n";

  // -------------------------------------------------------------
  // Test 2: Full-text Search
  // -------------------------------------------------------------
  std::cout << "\n[Test 2] Full-text Search via SearchCoordinator\n";
  auto ft_hits = coordinator.SearchFulltext("神经网络", 10, 0, "", &total_count);
  assert(ft_hits.size() == 1);
  assert(ft_hits[0].doc_id == 101);
  assert(!ft_hits[0].snippet.empty());
  std::cout << "[+] Full-text hit doc_id: " << ft_hits[0].doc_id << " | snippet: " << ft_hits[0].snippet << "\n";

  // -------------------------------------------------------------
  // Test 3: Multi-Source Hybrid Search & Ranking
  // -------------------------------------------------------------
  std::cout << "\n[Test 3] Multi-Source Hybrid Search & Ranking\n";
  // "人工智能" matches doc 102 in both filename and fulltext!
  auto hybrid_hits = coordinator.SearchHybrid("人工智能", 10, 0, "", &total_count);
  assert(total_count >= 1);
  assert(hybrid_hits[0].doc_id == 102);
  assert(hybrid_hits[0].is_filename_match);
  assert(hybrid_hits[0].is_fulltext_match);
  assert(hybrid_hits[0].score > 60.0);  // Boosted double match
  std::cout << "[+] Hybrid hit #1: " << hybrid_hits[0].file_name << " (score: "
            << hybrid_hits[0].score << ", snippet: " << hybrid_hits[0].snippet << ")\n";

  // -------------------------------------------------------------
  // Test 4: QueryToken Versioning & Cancellation
  // -------------------------------------------------------------
  std::cout << "\n[Test 4] QueryToken Generation & Invalidation\n";
  uint64_t t1 = coordinator.AllocateQueryToken();
  assert(coordinator.IsQueryTokenCurrent(t1));

  // User types another character
  uint64_t t2 = coordinator.AllocateQueryToken();
  assert(!coordinator.IsQueryTokenCurrent(t1));
  assert(coordinator.IsQueryTokenCurrent(t2));

  // Invalidate all
  coordinator.InvalidateActiveQueries();
  assert(!coordinator.IsQueryTokenCurrent(t2));
  std::cout << "[+] QueryToken versioning verified: t1=" << t1 << ", t2=" << t2 << " properly invalidated.\n";

  storage->Close();
  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());

  std::cout << "\n[ALL CHECKS PASSED] Stage 3 SearchCoordinator Unit Test Passed!\n";
  return 0;
}
