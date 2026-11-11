// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <gtest/gtest.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>
#include <string>

#include "common/machine_key_provider.h"
#include "storage/storage_engine.h"

namespace fs = std::filesystem;

class MachineKeyProviderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string test_name = info ? info->name() : "default";
    test_dir_ = fs::temp_directory_path() / ("soeasy_key_test_" + std::to_string(getpid()) + "_" + test_name);
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
    fs::create_directories(test_dir_, ec);
    dict_dir_ = "third_party/cppjieba/dict";
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(test_dir_, ec);
  }

  fs::path test_dir_;
  std::string dict_dir_;
};

// 1. Machine ID Stability: Must be non-empty, >= 32 chars, and 100% stable
TEST_F(MachineKeyProviderTest, MachineIdStabilityAndNonEmpty) {
  // Act
  std::string id1 = soeasy::common::MachineKeyProvider::GetMachineLifecycleId();
  std::string id2 = soeasy::common::MachineKeyProvider::GetMachineLifecycleId();

  // Assert
  EXPECT_FALSE(id1.empty());
  EXPECT_GE(id1.length(), 32u);
  EXPECT_EQ(id1, id2);
}

// 2. First-time Key Generation and 0600 Permissions
TEST_F(MachineKeyProviderTest, FirstTimeKeyGenerationAndPermissions) {
  // Arrange
  std::string app_data = test_dir_.string();
  fs::path key_path = test_dir_ / soeasy::common::MachineKeyProvider::kKeyFileName;
  EXPECT_FALSE(fs::exists(key_path));

  // Act
  auto result = soeasy::common::MachineKeyProvider::GetOrCreateDatabaseKey(app_data);

  // Assert
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.is_newly_generated);
  EXPECT_FALSE(result.encryption_key.empty());
  EXPECT_TRUE(fs::exists(key_path));

#ifndef _WIN32
  struct stat st;
  ASSERT_EQ(stat(key_path.c_str(), &st), 0);
  // Mode must be 0600 (S_IRUSR | S_IWUSR)
  mode_t file_perm = st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
  EXPECT_EQ(file_perm, S_IRUSR | S_IWUSR);
#endif
}

// 3. Subsequent Key Retrieval Idempotency
TEST_F(MachineKeyProviderTest, SubsequentKeyRetrievalIdempotency) {
  // Arrange
  std::string app_data = test_dir_.string();

  // Act: First call creates the key
  auto res1 = soeasy::common::MachineKeyProvider::GetOrCreateDatabaseKey(app_data);
  ASSERT_TRUE(res1.success);
  ASSERT_TRUE(res1.is_newly_generated);

  // Act: Second call reads existing sealed key
  auto res2 = soeasy::common::MachineKeyProvider::GetOrCreateDatabaseKey(app_data);

  // Assert
  EXPECT_TRUE(res2.success);
  EXPECT_FALSE(res2.is_newly_generated);
  EXPECT_EQ(res1.encryption_key, res2.encryption_key);
}

// 4. Foreign Machine Defense: Unsealing with mismatched machine_id must fail
TEST_F(MachineKeyProviderTest, ForeignMachineDefenseRejectsMismatchedId) {
  // Arrange
  std::string real_machine_id = soeasy::common::MachineKeyProvider::GetMachineLifecycleId();
  std::string foreign_machine_id = "0123456789abcdef0123456789abcdef";
  ASSERT_NE(real_machine_id, foreign_machine_id);

  std::string raw_dek = "soeasy_dek_super_secret_test_key_12345678";
  std::string sealed_envelope = soeasy::common::MachineKeyProvider::SealKey(raw_dek, real_machine_id);

  // Act: Unseal on the genuine machine
  std::string recovered_dek;
  bool genuine_ok = soeasy::common::MachineKeyProvider::UnsealKey(sealed_envelope, real_machine_id, &recovered_dek);
  EXPECT_TRUE(genuine_ok);
  EXPECT_EQ(recovered_dek, raw_dek);

  // Act: Attempt to unseal with foreign machine ID
  std::string foreign_recovered_dek;
  bool foreign_ok =
      soeasy::common::MachineKeyProvider::UnsealKey(sealed_envelope, foreign_machine_id, &foreign_recovered_dek);

  // Assert: Foreign unseal MUST fail and MUST NOT leak any DEK
  EXPECT_FALSE(foreign_ok);
  EXPECT_TRUE(foreign_recovered_dek.empty());
}

// 5. StorageEngine Integration with Machine Key
TEST_F(MachineKeyProviderTest, StorageEngineIntegrationWithMachineKey) {
  // Arrange
  auto key_result = soeasy::common::MachineKeyProvider::GetOrCreateDatabaseKey(test_dir_.string());
  ASSERT_TRUE(key_result.success);
  std::string machine_key = key_result.encryption_key;
  std::string db_path = (test_dir_ / "test_machine_bound.db").string();

  // Act: Open database and index document
  soeasy::storage::StorageEngine engine;
  ASSERT_TRUE(engine.Open(db_path, machine_key, dict_dir_));
  EXPECT_TRUE(engine.IndexDocument(1, "/home/user/doc.txt", "doc.txt", "txt", 100, 1000, 1, "保密测试内容"));
  engine.Close();

  // Assert: Reopen with machine key succeeds and can read document
  soeasy::storage::StorageEngine reopen_engine;
  ASSERT_TRUE(reopen_engine.Open(db_path, machine_key, dict_dir_));
  std::string content;
  EXPECT_TRUE(reopen_engine.GetDocumentContent(1, &content));
  EXPECT_EQ(content, "保密测试内容");
  reopen_engine.Close();

  // Assert: Opening with wrong key fails
  soeasy::storage::StorageEngine wrong_key_engine;
  EXPECT_FALSE(wrong_key_engine.Open(db_path, "WRONG_ATTACKER_KEY", dict_dir_));
}

// 6. Legacy Migration Test: Rekey from default legacy key to machine key
TEST_F(MachineKeyProviderTest, LegacyMigrationRekeyRoundtrip) {
  // Arrange
  std::string db_path = (test_dir_ / "legacy_migration.db").string();
  const std::string legacy_key = soeasy::common::MachineKeyProvider::kDefaultLegacyKey;

  // 1. Create legacy database with hardcoded key
  {
    soeasy::storage::StorageEngine legacy_engine;
    ASSERT_TRUE(legacy_engine.Open(db_path, legacy_key, dict_dir_));
    EXPECT_TRUE(
        legacy_engine.IndexDocument(42, "/data/report.docx", "report.docx", "docx", 500, 2000, 2, "重要财务数据"));
    legacy_engine.Close();
  }

  // 2. Obtain machine key
  auto key_result = soeasy::common::MachineKeyProvider::GetOrCreateDatabaseKey(test_dir_.string());
  ASSERT_TRUE(key_result.success);
  std::string machine_key = key_result.encryption_key;

  // 3. Open with legacy key and perform Rekey
  {
    soeasy::storage::StorageEngine migrate_engine;
    ASSERT_TRUE(migrate_engine.Open(db_path, legacy_key, dict_dir_));
    EXPECT_TRUE(migrate_engine.Rekey(machine_key));
    migrate_engine.Close();
  }

  // 4. Assert: Legacy key now FAILS to open the migrated database
  {
    soeasy::storage::StorageEngine old_key_engine;
    EXPECT_FALSE(old_key_engine.Open(db_path, legacy_key, dict_dir_));
  }

  // 5. Assert: Machine key SUCCEEDS and content is 100% intact
  {
    soeasy::storage::StorageEngine new_key_engine;
    ASSERT_TRUE(new_key_engine.Open(db_path, machine_key, dict_dir_));
    std::string content;
    EXPECT_TRUE(new_key_engine.GetDocumentContent(42, &content));
    EXPECT_EQ(content, "重要财务数据");
    new_key_engine.Close();
  }
}
