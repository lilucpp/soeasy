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

TEST_F(Fts5SyntaxTest, LargeDocumentChunkedTokenizationPreservesOffsetsAndPhrases) {
  // Construct > 120 KB bilingual text crossing multiple 32KB chunk boundaries
  std::string body;
  body.reserve(140 * 1024);
  const std::string line = "2026-11-20 INFO 桌面搜索引擎分块分词回归测试样本行 desktop search engine chunked tokenization.\n";
  while (body.size() < 31 * 1024) {
    body += line;
  }
  // Place a boundary-crossing phrase right around the 32KB line boundary
  body += "第一块末尾关键词 量子计算\n第二块起始关键词 异构加速\n";
  while (body.size() < 120 * 1024) {
    body += line;
  }
  body += "文档尾部唯一标识 麒麟架构 深度检索\n";

  DocumentIndexEntry entry;
  entry.doc_id = 101;
  entry.file_path = "/large/chunked_doc.txt";
  entry.file_name = "chunked_doc.txt";
  entry.ext = "txt";
  entry.file_size = body.size();
  entry.mtime = 1700000101;
  entry.parse_level = 1;
  entry.utf8_content = body;
  ASSERT_TRUE(engine_->IndexDocument(entry));

  // 1. Phrase query crossing the ~32KB chunk boundary
  int total_phrase = 0;
  auto hits_phrase = engine_->Search("\"量子计算 第二块起始关键词\"", 10, 0, "", &total_phrase);
  EXPECT_EQ(total_phrase, 1);
  ASSERT_EQ(hits_phrase.size(), 1u);
  EXPECT_EQ(hits_phrase[0].doc_id, 101);

  // 2. Tail keyword and NEAR query past 120KB (verifies later chunks and exact token positions)
  int total_tail = 0;
  auto hits_tail = engine_->Search("NEAR(麒麟架构 深度检索, 2)", 10, 0, "", &total_tail);
  EXPECT_EQ(total_tail, 1);
  ASSERT_EQ(hits_tail.size(), 1u);
  EXPECT_EQ(hits_tail[0].doc_id, 101);

  // 3. Single-character CJK colocated token past 120KB
  int total_char = 0;
  auto hits_char = engine_->Search("麟", 10, 0, "", &total_char);
  EXPECT_EQ(total_char, 1);
  ASSERT_EQ(hits_char.size(), 1u);
  EXPECT_EQ(hits_char[0].doc_id, 101);
}

TEST_F(Fts5SyntaxTest, AbnormalDelimiterFreeChunkIsDiscardedWhileSurroundingTextIsIndexed) {
  // Valid text before and after an abnormal > 40 KB delimiter-free blob (e.g. Base64 / hex dump)
  std::string body = "异常块前导正常文本天文学家\n";
  const std::string unit = "人工智能神经网络分布式检索";
  while (body.size() < 42 * 1024) {
    body += unit;
  }
  body += "超长无分隔符异常块内部词条星际穿越";
  body += "\n异常块后续正常文本量子力学\n";

  DocumentIndexEntry entry;
  entry.doc_id = 102;
  entry.file_path = "/large/no_newlines_utf8.txt";
  entry.file_name = "no_newlines_utf8.txt";
  entry.ext = "txt";
  entry.file_size = body.size();
  entry.mtime = 1700000102;
  entry.parse_level = 1;
  entry.utf8_content = body;
  ASSERT_TRUE(engine_->IndexDocument(entry));

  // Surrounding valid lines must be indexed
  int total_valid = 0;
  auto hits_valid = engine_->Search("天文学家 AND 量子力学", 10, 0, "", &total_valid);
  EXPECT_EQ(total_valid, 1);
  ASSERT_EQ(hits_valid.size(), 1u);
  EXPECT_EQ(hits_valid[0].doc_id, 102);

  // The > 40 KB delimiter-free abnormal blob must be discarded
  int total_discarded = 0;
  auto hits_discarded = engine_->Search("星际穿越", 10, 0, "", &total_discarded);
  EXPECT_EQ(total_discarded, 0);
  EXPECT_TRUE(hits_discarded.empty());
}

TEST_F(Fts5SyntaxTest, Exact32KBBoundaryFastPathAndChunkedPath) {
  const std::string line = "normal_line_padding_token_for_32kb_boundary_verification_012345\n";  // 64 bytes

  // 1. Exactly 32768 bytes (kTargetChunkBytes fast path)
  const std::string tail_exact = "精确边界尾词甲";
  std::string body_exact;
  body_exact.reserve(32768);
  while (body_exact.size() + line.size() <= 32768 - tail_exact.size() - 1) {
    body_exact += line;
  }
  body_exact.append(32768 - tail_exact.size() - 1 - body_exact.size(), 'a');
  body_exact += '\n';
  body_exact += tail_exact;
  ASSERT_EQ(body_exact.size(), 32768u);

  DocumentIndexEntry e1;
  e1.doc_id = 103;
  e1.file_path = "/large/exact_32k.txt";
  e1.file_name = "exact_32k.txt";
  e1.ext = "txt";
  e1.file_size = body_exact.size();
  e1.mtime = 1700000103;
  e1.utf8_content = body_exact;
  ASSERT_TRUE(engine_->IndexDocument(e1));

  // 2. Exactly 32769 bytes (1 byte over kTargetChunkBytes -> triggers chunked path)
  const std::string tail_over = "精确边界尾词乙";
  std::string body_over;
  body_over.reserve(32769);
  while (body_over.size() + line.size() <= 32769 - tail_over.size() - 1) {
    body_over += line;
  }
  body_over.append(32769 - tail_over.size() - 1 - body_over.size(), 'b');
  body_over += '\n';
  body_over += tail_over;
  ASSERT_EQ(body_over.size(), 32769u);

  DocumentIndexEntry e2;
  e2.doc_id = 104;
  e2.file_path = "/large/over_32k.txt";
  e2.file_name = "over_32k.txt";
  e2.ext = "txt";
  e2.file_size = body_over.size();
  e2.mtime = 1700000104;
  e2.utf8_content = body_over;
  ASSERT_TRUE(engine_->IndexDocument(e2));

  int t1 = 0;
  auto h1 = engine_->Search("精确边界尾词甲", 10, 0, "", &t1);
  EXPECT_EQ(t1, 1);
  ASSERT_EQ(h1.size(), 1u);
  EXPECT_EQ(h1[0].doc_id, 103);

  int t2 = 0;
  auto h2 = engine_->Search("精确边界尾词乙", 10, 0, "", &t2);
  EXPECT_EQ(t2, 1);
  ASSERT_EQ(h2.size(), 1u);
  EXPECT_EQ(h2[0].doc_id, 104);
}

TEST_F(Fts5SyntaxTest, Tier2CjkPunctuationBoundaryNoNewlines) {
  // > 40 KB Chinese text with NO newlines and NO spaces, only separated by Chinese punctuation (。；！)
  // Also embeds decimal/version tokens with '.' across the 32KB window to verify '.' is not a split delimiter.
  std::string body;
  body.reserve(48 * 1024);
  const std::string sentence = "搜索引擎采用双数组字典树实现高效中文分词与全文检索。";
  while (body.size() < 31 * 1024) {
    body += sentence;
  }
  body += "标点前词条玄武岩。标点后词条黑曜石；";
  while (body.size() < 32760) {
    body += "圆周率3.14159265358979323846版本；";
  }
  while (body.size() < 44 * 1024) {
    body += sentence;
  }

  DocumentIndexEntry entry;
  entry.doc_id = 105;
  entry.file_path = "/large/cjk_punct_only.txt";
  entry.file_name = "cjk_punct_only.txt";
  entry.ext = "txt";
  entry.file_size = body.size();
  entry.mtime = 1700000105;
  entry.utf8_content = body;
  ASSERT_TRUE(engine_->IndexDocument(entry));

  int total_near = 0;
  auto hits = engine_->Search("NEAR(玄武岩 黑曜石, 5)", 10, 0, "", &total_near);
  EXPECT_EQ(total_near, 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].doc_id, 105);

  int total_pi = 0;
  auto hits_pi = engine_->Search("\"3.14159265358979323846\"", 10, 0, "", &total_pi);
  EXPECT_EQ(total_pi, 1);
}

TEST_F(Fts5SyntaxTest, Tier2AsciiCommaBoundarySingleLineCsv) {
  // > 70 KB single-line CSV with NO newlines (exercises Tier 2 ASCII comma boundary)
  std::string body;
  body.reserve(75 * 1024);
  const std::string col = "record_id,timestamp_2026,status_ok,payload_alpha,";
  while (body.size() < 31 * 1024) {
    body += col;
  }
  body += "csv_mid_marker_apollo,csv_after_marker_artemis,";
  while (body.size() < 70 * 1024) {
    body += col;
  }
  body += "csv_tail_marker_voyager";

  DocumentIndexEntry entry;
  entry.doc_id = 106;
  entry.file_path = "/large/single_line.csv";
  entry.file_name = "single_line.csv";
  entry.ext = "csv";
  entry.file_size = body.size();
  entry.mtime = 1700000106;
  entry.utf8_content = body;
  ASSERT_TRUE(engine_->IndexDocument(entry));

  int t_mid = 0;
  auto h_mid = engine_->Search("NEAR(csv_mid_marker_apollo csv_after_marker_artemis, 3)", 10, 0, "", &t_mid);
  EXPECT_EQ(t_mid, 1);
  ASSERT_EQ(h_mid.size(), 1u);
  EXPECT_EQ(h_mid[0].doc_id, 106);

  int t_tail = 0;
  auto h_tail = engine_->Search("csv_tail_marker_voyager", 10, 0, "", &t_tail);
  EXPECT_EQ(t_tail, 1);
  ASSERT_EQ(h_tail.size(), 1u);
  EXPECT_EQ(h_tail[0].doc_id, 106);
}

TEST_F(Fts5SyntaxTest, PureDelimiterFreeLargeDocumentIsDiscardedSafely) {
  // Entire > 40 KB document has no spaces, newlines, or punctuation in [16KB, 32KB] -> discarded safely
  std::string body(32766, 'x');
  body += "无分隔符异常块词条凤凰涅槃";
  body.append(8000, 'y');

  DocumentIndexEntry entry;
  entry.doc_id = 107;
  entry.file_path = "/large/pure_delimiter_free.txt";
  entry.file_name = "pure_delimiter_free.txt";
  entry.ext = "txt";
  entry.file_size = body.size();
  entry.mtime = 1700000107;
  entry.utf8_content = body;
  ASSERT_TRUE(engine_->IndexDocument(entry));

  int total = 0;
  auto hits = engine_->Search("凤凰涅槃", 10, 0, "", &total);
  EXPECT_EQ(total, 0);
  EXPECT_TRUE(hits.empty());
}

TEST_F(Fts5SyntaxTest, InvalidUtf8ChunkIsDiscardedWhileValidChunksAreIndexed) {
  // First chunk (~30 KB) contains invalid UTF-8 bytes -> discarded;
  // Second chunk (~15 KB after CRLF split boundary) is valid UTF-8 -> indexed normally.
  std::string body;
  body.reserve(45 * 1024);
  const std::string line = "Windows CRLF line content 混合测试行\r\n";
  while (body.size() < 28 * 1024) {
    body += line;
  }
  body += "损坏分片内词条鲲鹏\xFF\xFE\x80\r\n";
  while (body.size() < 32 * 1024) {
    body += line;
  }
  // All bytes in [0, 32KB] belong to the first chunk (which contains invalid UTF-8);
  // tokens appended after 32KB belong to the second chunk (which is pure valid UTF-8).
  ASSERT_GE(body.size(), 32 * 1024u);

  body += "合法分片词条鸿蒙\r\n";
  while (body.size() < 42 * 1024) {
    body += line;
  }
  body += "结尾合法词条盘古\r\n";

  DocumentIndexEntry entry;
  entry.doc_id = 108;
  entry.file_path = "/large/invalid_utf8_crlf.txt";
  entry.file_name = "invalid_utf8_crlf.txt";
  entry.ext = "txt";
  entry.file_size = body.size();
  entry.mtime = 1700000108;
  entry.utf8_content = body;
  ASSERT_TRUE(engine_->IndexDocument(entry));

  // The first chunk containing invalid UTF-8 bytes was discarded
  int t_bad = 0;
  auto h_bad = engine_->Search("鲲鹏", 10, 0, "", &t_bad);
  EXPECT_EQ(t_bad, 0);
  EXPECT_TRUE(h_bad.empty());

  // The second chunk containing valid UTF-8 was indexed normally
  int t_good = 0;
  auto h_good = engine_->Search("鸿蒙 AND 盘古", 10, 0, "", &t_good);
  EXPECT_EQ(t_good, 1);
  ASSERT_EQ(h_good.size(), 1u);
  EXPECT_EQ(h_good[0].doc_id, 108);
}

}  // namespace test
}  // namespace soeasy
