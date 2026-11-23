// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "storage/istorage_engine.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

namespace soeasy {
namespace test {

using namespace soeasy::storage;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::StrictMock;

/**
 * @brief Test fixture for StorageEngine & TransactionGuard testing.
 */
class StorageTransactionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_db_path_ = (std::filesystem::temp_directory_path() /
                     ("soeasy_gtest_tx_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".db")).string();
    key_ = "SoEasy_GTest_Key_2026!";
    dict_dir_ = soeasy::test::FindDictDir();

    CleanUpFiles();

    engine_ = std::make_unique<StorageEngine>();
    ASSERT_TRUE(engine_->Open(test_db_path_, key_, dict_dir_));
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

  std::string test_db_path_;
  std::string key_;
  std::string dict_dir_;
  std::unique_ptr<StorageEngine> engine_;
};

// -----------------------------------------------------------------------------
// AAA Pattern Tests: TransactionGuard RAII Behavior
// -----------------------------------------------------------------------------

TEST_F(StorageTransactionTest, ExplicitCommitPersistsData) {
  // 1. Arrange
  const int64_t doc_id = 1001;
  const std::string file_path = "/docs/manual.docx";
  const std::string file_name = "manual.docx";
  const std::string content = "现代 C++ 自动化测试与持续集成实战指南";

  // 2. Act
  {
    TransactionGuard tx(*engine_);
    ASSERT_TRUE(tx.IsActive());

    bool inserted = engine_->IndexDocument(doc_id, file_path, file_name, "docx", 1024, 1700000000, 1, content);
    EXPECT_TRUE(inserted);

    EXPECT_TRUE(tx.Commit());
  }

  // 3. Assert: Data must be safely persisted
  auto results = engine_->Search("自动化测试", 10, 0);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].doc_id, doc_id);
  EXPECT_EQ(results[0].file_name, file_name);
}

TEST_F(StorageTransactionTest, AutoRollbackWhenOutOfScopeWithoutCommit) {
  // 1. Arrange
  const int64_t doc_id = 1002;
  const std::string file_path = "/docs/draft.docx";
  const std::string file_name = "draft.docx";
  const std::string content = "临时草稿内容，未经提交理应自动回滚";

  // 2. Act: Exit scope without calling tx.Commit()
  {
    TransactionGuard tx(*engine_);
    ASSERT_TRUE(tx.IsActive());

    bool inserted = engine_->IndexDocument(doc_id, file_path, file_name, "docx", 512, 1700000000, 1, content);
    EXPECT_TRUE(inserted);

    // Intentionally omit tx.Commit() - Destructor should trigger RollbackTransaction()
  }

  // 3. Assert: Data must NOT be present in database
  auto results = engine_->Search("临时草稿内容", 10, 0);
  EXPECT_TRUE(results.empty());

  uint64_t fsize = 0;
  int64_t mtime = 0;
  EXPECT_FALSE(engine_->GetFileMetadata(doc_id, &fsize, &mtime));
}

TEST_F(StorageTransactionTest, MultipleOperationsAtomicRollback) {
  // 1. Arrange: Insert baseline document
  const int64_t doc1 = 2001;
  const int64_t doc2 = 2002;
  {
    TransactionGuard tx(*engine_);
    engine_->IndexDocument(doc1, "/doc1.txt", "doc1.txt", "txt", 100, 1700000000, 1, "基线文档一");
    tx.Commit();
  }

  // 2. Act: In a new transaction, modify doc1, add doc2, then explicitly rollback
  {
    TransactionGuard tx(*engine_);
    engine_->IndexDocument(doc2, "/doc2.txt", "doc2.txt", "txt", 200, 1700000000, 1, "回滚文档二");
    tx.Rollback();
  }

  // 3. Assert: doc2 must not exist, doc1 must remain intact
  auto res_doc2 = engine_->Search("回滚文档二", 10, 0);
  EXPECT_TRUE(res_doc2.empty());

  auto res_doc1 = engine_->Search("基线文档一", 10, 0);
  ASSERT_EQ(res_doc1.size(), 1u);
  EXPECT_EQ(res_doc1[0].doc_id, doc1);
}

TEST_F(StorageTransactionTest, ParseIssuesAndStatusSummaryLifecycle) {
  ASSERT_TRUE(engine_->IndexDocument(101, "/docs/ok.txt", "ok.txt", "txt", 120, 1700000001, 1, "healthy body",
                                     static_cast<int>(common::ParseStatus::kOk), ""));
  ASSERT_TRUE(engine_->IndexDocument(102, "/docs/secret.pdf", "secret.pdf", "pdf", 2048, 1700000002, 1, "",
                                     static_cast<int>(common::ParseStatus::kEncrypted),
                                     "Document is encrypted and requires a password"));
  ASSERT_TRUE(engine_->IndexDocument(103, "/docs/huge.pdf", "huge.pdf", "pdf", 9999, 1700000003, 1, "",
                                     static_cast<int>(common::ParseStatus::kTimeout), "Task timed out after 60000 ms"));
  ASSERT_TRUE(engine_->IndexDocument(104, "/docs/crash.doc", "crash.doc", "doc", 512, 1700000004, 1, "",
                                     static_cast<int>(common::ParseStatus::kCrashed),
                                     "Worker crashed (SIGSEGV/abort) during file parsing"));
  ASSERT_TRUE(engine_->IndexDocument(105, "/docs/corrupt.docx", "corrupt.docx", "docx", 256, 1700000005, 1, "",
                                     static_cast<int>(common::ParseStatus::kFailed), "Central directory corrupt"));

  auto summary = engine_->GetParseStatusSummary();
  EXPECT_EQ(summary.total_files, 5u);
  EXPECT_EQ(summary.ok_count, 1u);
  EXPECT_EQ(summary.encrypted_count, 1u);
  EXPECT_EQ(summary.timeout_count, 1u);
  EXPECT_EQ(summary.crashed_count, 1u);
  EXPECT_EQ(summary.failed_count, 1u);
  EXPECT_EQ(summary.failure_count(), 3u);
  EXPECT_EQ(summary.issue_count(), 4u);

  const auto all_issues = engine_->GetParseIssues(std::nullopt, 50, 0);
  ASSERT_EQ(all_issues.size(), 4u);
  const auto enc_only = engine_->GetParseIssues(common::ParseStatus::kEncrypted, 50, 0);
  ASSERT_EQ(enc_only.size(), 1u);
  EXPECT_EQ(enc_only[0].file_path, "/docs/secret.pdf");
  EXPECT_EQ(enc_only[0].reason, "Document is encrypted and requires a password");

  // Re-indexing doc 105 cleanly removes it from parse_issues and increments ok_count
  ASSERT_TRUE(engine_->IndexDocument(105, "/docs/corrupt.docx", "corrupt.docx", "docx", 300, 1700000006, 1,
                                     "Recovered docx text", static_cast<int>(common::ParseStatus::kOk), ""));
  summary = engine_->GetParseStatusSummary();
  EXPECT_EQ(summary.total_files, 5u);
  EXPECT_EQ(summary.ok_count, 2u);
  EXPECT_EQ(summary.failed_count, 0u);
  EXPECT_EQ(engine_->GetParseIssues(common::ParseStatus::kFailed, 50, 0).size(), 0u);

  // Deleting doc 102 cascades out of parse_issues
  ASSERT_TRUE(engine_->DeleteDocument(102));
  summary = engine_->GetParseStatusSummary();
  EXPECT_EQ(summary.total_files, 4u);
  EXPECT_EQ(summary.encrypted_count, 0u);

  // ClearAll resets both files and parse_issues
  ASSERT_TRUE(engine_->ClearAll());
  summary = engine_->GetParseStatusSummary();
  EXPECT_EQ(summary.total_files, 0u);
  EXPECT_EQ(summary.issue_count(), 0u);

  // Invalid parse_stat normalizes to kOk (no drift), and invalid UTF-8 bytes in error_reason are sanitized
  ASSERT_TRUE(engine_->IndexDocument(106, "/docs/invalid_stat.txt", "invalid_stat.txt", "txt", 10, 1700000007, 1,
                                     "normal body", /*parse_stat=*/99, "ignored"));
  int stored_stat = 0;
  ASSERT_TRUE(engine_->GetFileMetadata(106, nullptr, nullptr, &stored_stat));
  EXPECT_EQ(stored_stat, static_cast<int>(common::ParseStatus::kOk));
  EXPECT_TRUE(engine_->GetParseIssues(std::nullopt, 50, 0).empty());

  const std::string bad_utf8_reason = std::string("Worker diag: \xFF\xFE\x80 trailing");
  ASSERT_TRUE(engine_->IndexDocument(107, "/docs/bad_utf8.pdf", "bad_utf8.pdf", "pdf", 10, 1700000008, 1, "",
                                     static_cast<int>(common::ParseStatus::kCrashed), bad_utf8_reason));
  const auto crash_rows = engine_->GetParseIssues(common::ParseStatus::kCrashed, 50, 0);
  ASSERT_EQ(crash_rows.size(), 1u);
  EXPECT_NE(crash_rows[0].reason.find("Worker diag:"), std::string::npos);
  EXPECT_EQ(crash_rows[0].reason.find('\xFF'), std::string::npos);
}

// -----------------------------------------------------------------------------
// GoogleMock Dependency Injection Test: Contract Verification
// -----------------------------------------------------------------------------

class MockStorageEngineContract : public IStorageEngine {
 public:
  MOCK_METHOD(bool, DoOpen, (const std::string &, const std::string &, const std::string &), (override));
  MOCK_METHOD(void, DoClose, (), (override));
  MOCK_METHOD(bool, DoIsOpen, (), (const, override));
  MOCK_METHOD(bool, DoBeginTransaction, (), (override));
  MOCK_METHOD(bool, DoCommitTransaction, (), (override));
  MOCK_METHOD(bool, DoRollbackTransaction, (), (override));
  MOCK_METHOD(bool, DoIsInTransaction, (), (const, override));
  MOCK_METHOD(size_t, DoGetUncommittedCount, (), (const, override));
  MOCK_METHOD(bool, DoOptimizeFtsIndex, (), (override));
  MOCK_METHOD(bool, DoSetBulkMode, (bool), (override));
  MOCK_METHOD(bool, DoCheckpointWal, (), (override));
  MOCK_METHOD(bool, DoGetFragmentation, (StorageFragmentation *), (override));
  MOCK_METHOD(bool, DoIncrementalVacuum, (int), (override));
  MOCK_METHOD(bool, DoVacuum, (), (override));
  MOCK_METHOD(bool, DoIndexDocument, (const DocumentIndexEntry &), (override));
  MOCK_METHOD(bool, DoDeleteDocument, (int64_t), (override));
  MOCK_METHOD(int64_t, DoDeleteDirectory, (const std::string &, std::vector<int64_t> *), (override));
  MOCK_METHOD(bool, DoClearAll, (), (override));
  MOCK_METHOD(std::vector<SearchResultItem>, DoSearch, (const std::string &, int, int, const std::string &, int *),
              (override));
  MOCK_METHOD(bool, DoGetDocumentContent, (int64_t, std::string *), (override));
  MOCK_METHOD(bool, DoGetFileMetadata, (int64_t, uint64_t *, int64_t *, int *), (override));
  MOCK_METHOD(std::vector<FileMetaRecord>, DoGetAllFilesMetadata, (), (override));
  MOCK_METHOD(ScopedReadConnection, DoAcquireReadConnection, (), (override));
  MOCK_METHOD(bool, DoRekey, (const std::string &), (override));
  MOCK_METHOD(std::string, DoGetLastError, (), (const, override));
};

TEST(TransactionGuardMockTest, EnforcesRollbackOnExceptionPath) {
  // Arrange: Set up StrictMock expectation
  StrictMock<MockStorageEngineContract> mock_engine;

  EXPECT_CALL(mock_engine, DoBeginTransaction()).WillOnce(Return(true));
  EXPECT_CALL(mock_engine, DoRollbackTransaction()).Times(1).WillOnce(Return(true));

  // Act: Simulate an unexpected failure before commit
  try {
    TransactionGuard guard(mock_engine);
    ASSERT_TRUE(guard.IsActive());
    throw std::runtime_error("Simulated catastrophic business error during batch insert");
  } catch (const std::exception &ex) {
    // Assert: Handled exception, GMock automatically verifies RollbackTransaction() was invoked on guard destruction
    EXPECT_STREQ(ex.what(), "Simulated catastrophic business error during batch insert");
  }
}

}  // namespace test
}  // namespace soeasy
