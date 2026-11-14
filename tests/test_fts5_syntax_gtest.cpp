// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/snippet_highlighter.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

namespace soeasy {
namespace test {

using storage::DocumentIndexEntry;
using storage::StorageEngine;

class Fts5SyntaxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_db_path_ = (std::filesystem::temp_directory_path() /
                     ("soeasy_fts5_syntax_" + std::to_string(soeasy::test::GetProcessIdCompat()) + "_" +
                      std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_" +
                      std::to_string(reinterpret_cast<uintptr_t>(this)) + ".db")).string();
    key_ = "Fts5_GTest_Secret_Key_2026!";
    dict_dir_ = soeasy::test::FindDictDir();

    CleanUpFiles();

    engine_ = std::make_unique<StorageEngine>();
    ASSERT_TRUE(engine_->Open(test_db_path_, key_, dict_dir_));

    SeedSampleDocuments();
  }

  void TearDown() override {
    if (engine_) {
      engine_->Close();
      engine_.reset();
    }
    CleanUpFiles();
  }

  void CleanUpFiles() {
    std::filesystem::remove(test_db_path_);
    std::filesystem::remove(test_db_path_ + "-wal");
    std::filesystem::remove(test_db_path_ + "-shm");
  }

  void SeedSampleDocuments() {
    struct DocSeed {
      int64_t id;
      std::string path;
      std::string name;
      std::string ext;
      std::string content;
    };

    std::vector<DocSeed> docs = {
        {1, "/docs/sqlite_arch.md", "sqlite_arch.md", "md",
         "sqlite database architecture and high performance indexing with btree"},
        {2, "/docs/distributed_storage.md", "distributed_storage.md", "md",
         "distributed storage and mysql replication guide for cloud backend"},
        {3, "/docs/cpp_concurrency.cpp", "cpp_concurrency.cpp", "cpp",
         "modern c++ concurrency in action with multithreading and lock-free atomic data structures"},
        {4, "/docs/ml_pytorch.py", "ml_pytorch.py", "py",
         "python machine learning and deep neural networks with pytorch and tensor operations"},
        {5, "/docs/csapp_memory.txt", "csapp_memory.txt", "txt",
         "深入理解计算机系统：物理内存管理、虚拟地址转换与页表机制详解"},
        {6, "/docs/search_engine.txt", "search_engine.txt", "txt",
         "高性能分布式搜索引擎设计与倒排索引实现原理，支持海量数据高并发检索"},
        {7, "/docs/cpp_testing.md", "cpp_testing.md", "md",
         "现代 C++ 自动化测试与持续集成实战指南，基于 GoogleTest 与 CTest 流水线"},
        {8, "/docs/sqlite_fts5_jieba.txt", "sqlite_fts5_jieba.txt", "txt",
         "基于 SQLite FTS5 的全文检索与分词优化技术方案，结合结巴中文分词引擎"},
        {9, "/docs/linux_kernel.c", "linux_kernel.c", "c",
         "Linux 操作系统内核调试：使用 GDB 与 eBPF 进行系统调用跟踪与性能剖析"},
        {10, "/docs/soeasy_readme.md", "soeasy_readme.md", "md",
         "SoEasy 本地极速桌面搜索工具：使用 Qt6、SQLite3MC 和 Zstandard 压缩存储"}
    };

    ASSERT_TRUE(engine_->BeginTransaction());
    for (const auto &d : docs) {
      DocumentIndexEntry entry;
      entry.doc_id = d.id;
      entry.file_path = d.path;
      entry.file_name = d.name;
      entry.ext = d.ext;
      entry.file_size = d.content.size();
      entry.mtime = 1700000000 + d.id;
      entry.parse_level = 1;
      entry.utf8_content = d.content;
      EXPECT_TRUE(engine_->IndexDocument(entry));
    }
    ASSERT_TRUE(engine_->CommitTransaction());
  }

  std::string test_db_path_;
  std::string key_;
  std::string dict_dir_;
  std::unique_ptr<StorageEngine> engine_;
};

// -----------------------------------------------------------------------------
// Category 1: Single Term Queries
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, EnglishSingleTermQuery) {
  int total = 0;
  auto hits = engine_->Search("database", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 1);
  std::string content;
  ASSERT_TRUE(engine_->GetDocumentContent(hits[0].doc_id, &content));
  auto snippet = common::SnippetHighlighter::GenerateSnippet(
      content, common::SnippetHighlighter::ExtractKeywordsFromQuery("database"), 140);
  EXPECT_NE(snippet.find("<mark>database</mark>"), std::string::npos);
}

TEST_F(Fts5SyntaxTest, ChineseSingleTermQuery) {
  int total = 0;
  auto hits = engine_->Search("内存", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 5);
  std::string content;
  ASSERT_TRUE(engine_->GetDocumentContent(hits[0].doc_id, &content));
  auto snippet = common::SnippetHighlighter::GenerateSnippet(
      content, common::SnippetHighlighter::ExtractKeywordsFromQuery("内存"), 140);
  EXPECT_NE(snippet.find("<mark>内存</mark>"), std::string::npos);
}

// -----------------------------------------------------------------------------
// Category 2: Implicit AND Queries
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, ImplicitAndQueryEnglishAndChinese) {
  int total_en = 0;
  auto hits_en = engine_->Search("sqlite indexing", 10, 0, "", &total_en);
  EXPECT_EQ(total_en, 1);
  ASSERT_EQ(hits_en.size(), 1u);
  EXPECT_EQ(hits_en[0].doc_id, 1);

  int total_zh = 0;
  auto hits_zh = engine_->Search("分布式 倒排索引", 10, 0, "", &total_zh);
  EXPECT_EQ(total_zh, 1);
  ASSERT_EQ(hits_zh.size(), 1u);
  EXPECT_EQ(hits_zh[0].doc_id, 6);
}

// -----------------------------------------------------------------------------
// Category 3: Explicit Boolean Queries
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, ExplicitBooleanAnd) {
  int total = 0;
  auto hits = engine_->Search("sqlite AND architecture", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 1);
}

TEST_F(Fts5SyntaxTest, ExplicitBooleanOr) {
  int total = 0;
  auto hits = engine_->Search("mysql OR pytorch", 10, 0, "", &total);
  EXPECT_EQ(total, 2);
  EXPECT_EQ(hits.size(), 2u);
}

TEST_F(Fts5SyntaxTest, ExplicitBooleanNot) {
  int total = 0;
  auto hits = engine_->Search("全文检索 NOT 结巴", 10, 0, "", &total);
  // Doc 8 has both 全文检索 and 结巴, so NOT excludes it
  EXPECT_EQ(total, 0);
  EXPECT_TRUE(hits.empty());
}

// -----------------------------------------------------------------------------
// Category 4: Prefix Queries
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, PrefixWildcardEnglishAndChinese) {
  int total_en = 0;
  auto hits_en = engine_->Search("data*", 10, 0, "", &total_en);
  // Hits doc 1 (database) and doc 3 (data)
  EXPECT_GE(total_en, 2);

  int total_zh = 0;
  auto hits_zh = engine_->Search("搜索*", 10, 0, "", &total_zh);
  // Hits doc 6 (搜索引擎) and doc 10 (搜索)
  EXPECT_GE(total_zh, 2);
}

// -----------------------------------------------------------------------------
// Category 5: Phrase Queries
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, ExactPhraseQuery) {
  int total = 0;
  auto hits = engine_->Search("\"虚拟地址转换\"", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 5);
}

// -----------------------------------------------------------------------------
// Category 6: Column Filter Queries
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, ColumnFilterQueries) {
  int total_body = 0;
  auto hits_body = engine_->Search("body: 倒排索引", 10, 0, "", &total_body);
  EXPECT_EQ(total_body, 1);
  ASSERT_EQ(hits_body.size(), 1u);
  EXPECT_EQ(hits_body[0].doc_id, 6);

  int total_fn = 0;
  auto hits_fn = engine_->Search("file_name: \"cpp_concurrency.cpp\"", 10, 0, "", &total_fn);
  EXPECT_EQ(total_fn, 1);
  ASSERT_EQ(hits_fn.size(), 1u);
  EXPECT_EQ(hits_fn[0].doc_id, 3);
}

// -----------------------------------------------------------------------------
// Category 7: NEAR Queries
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, NearQueries) {
  int total = 0;
  auto hits = engine_->Search("NEAR(全文检索 分词, 5)", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 8);
}

// -----------------------------------------------------------------------------
// Category 8: Parentheses Grouping
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, ParenthesesGrouping) {
  int total = 0;
  auto hits = engine_->Search("(sqlite OR mysql) AND storage", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 2);
}

// -----------------------------------------------------------------------------
// Category 9: Chinese Continuous Sentence
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, ChineseContinuousSentenceWithoutSpaces) {
  int total = 0;
  auto hits = engine_->Search("高性能分布式搜索引擎", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 6);
}

// -----------------------------------------------------------------------------
// Category 10: Graceful Fallback for Special Symbols & Syntax Boundary Cases
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, FallbackRecoversCPlusPlusQuery) {
  // "C++" triggers FTS5 syntax error on '+' outside quotes.
  // Our fallback mechanism must seamlessly recover and find doc 3 and doc 7!
  int total = 0;
  auto hits = engine_->Search("C++", 10, 0, "", &total);
  EXPECT_GE(total, 2);
  EXPECT_GE(hits.size(), 2u);
  std::string content;
  ASSERT_TRUE(engine_->GetDocumentContent(hits[0].doc_id, &content));
  auto snippet = common::SnippetHighlighter::GenerateSnippet(
      content, common::SnippetHighlighter::ExtractKeywordsFromQuery("C++"), 140);
  EXPECT_NE(snippet.find("<mark>c++</mark>"), std::string::npos);
}

TEST_F(Fts5SyntaxTest, FallbackRecoversHyphenatedQuery) {
  // "lock-free" contains '-' which is unary NOT in FTS5.
  // Fallback must treat it as literal phrase and match doc 3!
  int total = 0;
  auto hits = engine_->Search("lock-free", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 3);
  std::string content;
  ASSERT_TRUE(engine_->GetDocumentContent(hits[0].doc_id, &content));
  auto snippet = common::SnippetHighlighter::GenerateSnippet(
      content, common::SnippetHighlighter::ExtractKeywordsFromQuery("lock-free"), 140);
  EXPECT_NE(snippet.find("<mark>lock-free</mark>"), std::string::npos);
}

TEST_F(Fts5SyntaxTest, FallbackRecoversMixedCPlusPlusAndChinese) {
  int total = 0;
  auto hits = engine_->Search("C++ 自动化测试", 10, 0, "", &total);
  EXPECT_EQ(total, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 7);
}

TEST_F(Fts5SyntaxTest, FallbackHandlesUnclosedQuotesSafely) {
  int total = 0;
  auto hits = engine_->Search("\"unclosed quote", 10, 0, "", &total);
  // Must not crash or error; returns 0 hits safely
  EXPECT_EQ(total, 0);
  EXPECT_TRUE(hits.empty());
}

TEST_F(Fts5SyntaxTest, FallbackHandlesPureOperatorsSafely) {
  int total_and = 0;
  EXPECT_NO_THROW({
    engine_->Search("AND", 10, 0, "", &total_and);
    engine_->Search("OR", 10, 0, "", &total_and);
    engine_->Search("NOT", 10, 0, "", &total_and);
    engine_->Search("database AND", 10, 0, "", &total_and);
  });
}

TEST_F(Fts5SyntaxTest, FallbackHandlesPurePunctuationSafely) {
  int total = 0;
  EXPECT_NO_THROW({
    engine_->Search("***", 10, 0, "", &total);
    engine_->Search("???", 10, 0, "", &total);
  });
}

// -----------------------------------------------------------------------------
// Category 11: SQL Injection Defense & LIKE Wildcard Escaping
// -----------------------------------------------------------------------------

TEST_F(Fts5SyntaxTest, ClassicSqlInjectionPayloadsAreNeutralized) {
  std::vector<std::string> attack_payloads = {
      "' OR 1=1 --",
      "admin' --",
      "' UNION SELECT 999, 'hacked.txt', '/hacked', 'txt', 0, 0, 0.0 --",
      "'; DROP TABLE files; --",
      "'; DELETE FROM fts_docs; --",
      "1' AND (SELECT count(*) FROM files) > 0 --",
      "\") OR (\"1\"=\"1",
      "' OR '1'='1",
      "' OR EXISTS(SELECT * FROM files) --"
  };

  for (const auto &payload : attack_payloads) {
    int total = -1;
    auto hits = engine_->Search(payload, 10, 0, "", &total);
    // Must return 0 hits, must not throw, must not drop any table
    EXPECT_EQ(hits.size(), 0u);
    EXPECT_EQ(total, 0);
  }

  // Ensure files and contents tables are intact
  auto all_meta = engine_->GetAllFilesMetadata();
  EXPECT_EQ(all_meta.size(), 10u);
}

TEST_F(Fts5SyntaxTest, LikeWildcardInPathPrefixIsEscaped) {
  // Seed a document with '%' and '_' in path
  DocumentIndexEntry entry;
  entry.doc_id = 99;
  entry.file_path = "/special/100%_done/report.txt";
  entry.file_name = "report.txt";
  entry.ext = "txt";
  entry.file_size = 50;
  entry.mtime = 1700000099;
  entry.parse_level = 1;
  entry.utf8_content = "special report for 100 percent progress";
  ASSERT_TRUE(engine_->IndexDocument(entry));

  // 1. Search with exact matching prefix "/special/100%_done" -> matches doc 99
  int total_match = 0;
  auto hits_match = engine_->Search("report", 10, 0, "/special/100%_done", &total_match);
  EXPECT_EQ(total_match, 1);
  ASSERT_EQ(hits_match.size(), 1u);
  EXPECT_EQ(hits_match[0].doc_id, 99);

  // 2. Search with unescaped prefix "/special/100" -> does not falsely match if we restrict
  int total_other = 0;
  auto hits_other = engine_->Search("report", 10, 0, "/special/100X", &total_other);
  EXPECT_EQ(total_other, 0);
  EXPECT_TRUE(hits_other.empty());
}

}  // namespace test
}  // namespace soeasy
