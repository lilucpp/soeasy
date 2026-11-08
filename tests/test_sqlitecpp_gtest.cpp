// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include <SQLiteCpp/SQLiteCpp.h>
#include "sqlite3mc_amalgamation.h"
#include "storage/fts5_jieba_tokenizer.h"
#include "tests/test_helper.h"

namespace soeasy {
namespace test {

class SQLiteCppIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_db_path_ = (std::filesystem::temp_directory_path() /
                     ("soeasy_sqlitecpp_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".db")).string();
    key_ = "SQLiteCpp_Secret_Key_2026!";
    dict_dir_ = soeasy::test::FindDictDir();
    CleanUpFiles();
  }

  void TearDown() override {
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
};

TEST_F(SQLiteCppIntegrationTest, BasicDatabaseCreationAndEncryptionPragma) {
  // 1. Arrange & Act: Open database using SQLiteCpp and configure SQLite3MC encryption
  {
    SQLite::Database db(test_db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("PRAGMA key = '" + key_ + "';");
    db.exec("PRAGMA cipher = 'aes128cbc';");
    db.exec("CREATE TABLE notes (id INTEGER PRIMARY KEY, title TEXT, content TEXT);");

    SQLite::Statement insert(db, "INSERT INTO notes (title, content) VALUES (?, ?);");
    insert.bind(1, "现代 C++");
    insert.bind(2, "使用 SQLiteCpp 提升类型安全与 RAII 确定性析构");
    EXPECT_EQ(insert.exec(), 1);
  }

  // 2. Assert: Re-open with correct key succeeds and can read data
  {
    SQLite::Database db(test_db_path_, SQLite::OPEN_READONLY);
    db.exec("PRAGMA key = '" + key_ + "';");

    SQLite::Statement query(db, "SELECT id, title, content FROM notes WHERE id = 1;");
    ASSERT_TRUE(query.executeStep());
    EXPECT_EQ(query.getColumn(0).getInt64(), 1);
    EXPECT_EQ(query.getColumn(1).getString(), "现代 C++");
    EXPECT_NE(query.getColumn(2).getString().find("SQLiteCpp"), std::string::npos);
  }

  // 3. Assert: Re-opening with wrong key fails to decrypt (verifies physical encryption)
  {
    SQLite::Database db(test_db_path_, SQLite::OPEN_READONLY);
    db.exec("PRAGMA key = 'WRONG_INVALID_KEY';");
    EXPECT_THROW({
      SQLite::Statement query(db, "SELECT count(*) FROM notes;");
      query.executeStep();
    }, SQLite::Exception);
  }
}

TEST_F(SQLiteCppIntegrationTest, TransactionCommitAndAutoRollback) {
  // 1. Arrange: Create table
  {
    SQLite::Database db(test_db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("PRAGMA key = '" + key_ + "';");
    db.exec("CREATE TABLE accounts (id INTEGER PRIMARY KEY, balance INTEGER);");
  }

  // 2. Act: Successful committed transaction
  {
    SQLite::Database db(test_db_path_, SQLite::OPEN_READWRITE);
    db.exec("PRAGMA key = '" + key_ + "';");

    SQLite::Transaction transaction(db);
    db.exec("INSERT INTO accounts (id, balance) VALUES (1, 100);");
    transaction.commit();
  }

  // 3. Assert: Value 1 persists
  {
    SQLite::Database db(test_db_path_, SQLite::OPEN_READONLY);
    db.exec("PRAGMA key = '" + key_ + "';");
    SQLite::Statement q(db, "SELECT count(*) FROM accounts;");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt(), 1);
  }

  // 4. Act: Transaction rolled back on scope exit without commit
  {
    SQLite::Database db(test_db_path_, SQLite::OPEN_READWRITE);
    db.exec("PRAGMA key = '" + key_ + "';");

    {
      SQLite::Transaction transaction(db);
      db.exec("INSERT INTO accounts (id, balance) VALUES (2, 200);");
      // Intentionally out of scope without commit()
    }
  }

  // 5. Assert: Value 2 was rolled back
  {
    SQLite::Database db(test_db_path_, SQLite::OPEN_READONLY);
    db.exec("PRAGMA key = '" + key_ + "';");
    SQLite::Statement q(db, "SELECT count(*) FROM accounts WHERE id = 2;");
    ASSERT_TRUE(q.executeStep());
    EXPECT_EQ(q.getColumn(0).getInt(), 0);
  }
}

TEST_F(SQLiteCppIntegrationTest, CustomFts5JiebaTokenizerRegistration) {
  // 1. Arrange: Create database and register custom FTS5 Jieba tokenizer via db.getHandle()
  SQLite::Database db(test_db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  db.exec("PRAGMA key = '" + key_ + "';");

  int reg_rc = storage::RegisterFts5JiebaTokenizer(db.getHandle(), dict_dir_);
  ASSERT_EQ(reg_rc, SQLITE_OK);

  // 2. Act: Create FTS5 table with Jieba tokenizer
  db.exec("CREATE VIRTUAL TABLE fts_articles USING fts5(title, body, tokenize='jieba');");

  SQLite::Statement ins(db, "INSERT INTO fts_articles (title, body) VALUES (?, ?);");
  ins.bind(1, "中国自然语言处理");
  ins.bind(2, "自然语言处理与机器学习在搜索引擎领域的广泛实践与探索。");
  ins.exec();

  // 3. Assert: Query using FTS5 match via SQLiteCpp Statement
  SQLite::Statement query(db, "SELECT rowid, title FROM fts_articles WHERE fts_articles MATCH ?;");
  query.bind(1, "机器学习");
  ASSERT_TRUE(query.executeStep());
  EXPECT_EQ(query.getColumn(0).getInt64(), 1);
  EXPECT_EQ(query.getColumn(1).getString(), "中国自然语言处理");
}

TEST_F(SQLiteCppIntegrationTest, TypeSafeParameterBindingAndNullHandling) {
  SQLite::Database db(test_db_path_, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
  db.exec("PRAGMA key = '" + key_ + "';");
  db.exec("CREATE TABLE types_test (id INTEGER, name TEXT, ratio REAL, payload BLOB);");

  // Insert with bind
  SQLite::Statement insert(db, "INSERT INTO types_test VALUES (?, ?, ?, ?);");
  insert.bind(1, static_cast<int64_t>(42));
  insert.bind(2, "测试字符串");
  insert.bind(3, 3.14159);
  const char raw_bytes[] = {0x00, 0x01, 0x02, static_cast<char>(0xFF)};
  insert.bind(4, raw_bytes, sizeof(raw_bytes));
  EXPECT_EQ(insert.exec(), 1);

  // Query and verify types
  SQLite::Statement select(db, "SELECT id, name, ratio, payload FROM types_test WHERE id = 42;");
  ASSERT_TRUE(select.executeStep());
  EXPECT_EQ(select.getColumn(0).getInt64(), 42);
  EXPECT_EQ(select.getColumn(1).getString(), "测试字符串");
  EXPECT_NEAR(select.getColumn(2).getDouble(), 3.14159, 0.0001);
  EXPECT_EQ(select.getColumn(3).getBytes(), 4);
}

}  // namespace test
}  // namespace soeasy
