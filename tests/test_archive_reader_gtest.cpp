#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/encoding_utils.h"
#include "common/format_categories.h"
#include "common/parse_status.h"
#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"
#include "worker/archive_reader.h"

namespace {

void WriteBinaryFile(const std::filesystem::path &path, const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string RunPythonScript(const std::string &script) {
  const std::filesystem::path tmp = soeasy::test::GetTestTempPath("gen_archive_script.py");
  WriteBinaryFile(tmp, script);
  std::string cmd = "python3 \"" + tmp.string() + "\"";
  const int rc = std::system(cmd.c_str());
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  return rc == 0 ? "" : ("python3 failed rc=" + std::to_string(rc));
}

class ArchiveReaderIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    root_ = soeasy::test::GetTestTempPath("soeasy_arch_test_" + stamp);
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(root_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  std::filesystem::path root_;
};

TEST_F(ArchiveReaderIntegrationTest, ExtractsAllSupportedFormatsAndNestedContainers) {
  const std::string py = R"PY(
import bz2, gzip, lzma, os, subprocess, tarfile, zipfile
root = r""")PY" + root_.string() +
                         R"PY("""

def write_tar(path, mode, arc_name, data):
    raw_src = os.path.join(root, "_tmp_src.bin")
    with open(raw_src, "wb") as f:
        f.write(data)
    with tarfile.open(path, mode) as tf:
        tf.add(raw_src, arcname=arc_name)
    os.remove(raw_src)

# 1. ZIP with Chinese names + unselected ext + oversized ext
with zipfile.ZipFile(os.path.join(root, "sample.zip"), "w", zipfile.ZIP_DEFLATED) as zf:
    zf.writestr("中文目录/合同正文.txt", "SoEasyArchiveZipToken 中文归档".encode("utf-8"))
    zf.writestr("ignored.exe", b"MZ\x90\x00skip_me")
    zf.writestr("oversized.txt", b"X" * 4096)

# 2. TAR / TGZ / TBZ2 / TXZ
write_tar(os.path.join(root, "sample.tar"), "w", "notes.txt", b"TarTokenAlpha")
write_tar(os.path.join(root, "sample.tgz"), "w:gz", "sub/notes.md", b"TgzTokenBeta")
write_tar(os.path.join(root, "sample.tbz2"), "w:bz2", "notes.txt", b"Tbz2TokenGamma")
write_tar(os.path.join(root, "sample.txz"), "w:xz", "notes.txt", b"TxzTokenDelta")

# 3. Single-file GZ / BZ2 / XZ / LZMA
with gzip.open(os.path.join(root, "single.txt.gz"), "wb") as f:
    f.write(b"SingleGzTokenOne")
with open(os.path.join(root, "single.txt.bz2"), "wb") as f:
    f.write(bz2.compress(b"SingleBz2TokenTwo"))
with open(os.path.join(root, "single.txt.xz"), "wb") as f:
    f.write(lzma.compress(b"SingleXzTokenThree", format=lzma.FORMAT_XZ))
with open(os.path.join(root, "single.txt.lzma"), "wb") as f:
    f.write(lzma.compress(b"SingleLzmaTokenFour", format=lzma.FORMAT_ALONE))

# 4. 3-layer nested: inner.7z (if 7z exists) -> mid.tar.gz -> outer.zip
deep_leaf = os.path.join(root, "deep_leaf.txt")
with open(deep_leaf, "wb") as f:
    f.write(b"Nested3LayerTokenDeep")
inner_7z = os.path.join(root, "inner.7z")
subprocess.check_call(["7z", "a", "-y", inner_7z, deep_leaf], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
mid_tgz = os.path.join(root, "mid.tar.gz")
with tarfile.open(mid_tgz, "w:gz") as tf:
    tf.add(inner_7z, arcname="inner.7z")
with zipfile.ZipFile(os.path.join(root, "nested_3layer.zip"), "w") as zf:
    zf.write(mid_tgz, arcname="layer1/mid.tar.gz")
    zf.writestr("top.txt", b"TopLevelAlongsideNested")
)PY";
  ASSERT_EQ(RunPythonScript(py), "");

  soeasy::worker::ArchiveReader reader;
  soeasy::worker::ArchiveExtractOptions opts;
  opts.enabled_exts = {"txt", "md"};
  opts.max_entry_bytes = 2048;

  struct Case {
    std::string file;
    std::string expected_substr;
    bool expect_skipped_large = false;
  };
  const std::vector<Case> cases = {
      {"sample.zip", "SoEasyArchiveZipToken", true},
      {"sample.tar", "TarTokenAlpha", false},
      {"sample.tgz", "TgzTokenBeta", false},
      {"sample.tbz2", "Tbz2TokenGamma", false},
      {"sample.txz", "TxzTokenDelta", false},
      {"single.txt.gz", "SingleGzTokenOne", false},
      {"single.txt.bz2", "SingleBz2TokenTwo", false},
      {"single.txt.xz", "SingleXzTokenThree", false},
      {"single.txt.lzma", "SingleLzmaTokenFour", false},
      {"nested_3layer.zip", "Nested3LayerTokenDeep", false},
  };

  for (const auto &c : cases) {
    bool found_expected = false;
    bool saw_skipped_large = false;
    bool saw_ignored_exe = false;
    std::string err;
    const std::string arch_path = (root_ / c.file).string();
    EXPECT_TRUE(reader.Extract(
        arch_path, opts,
        [&](const soeasy::worker::ArchiveItem &item) {
          if (item.ext == "exe") {
            saw_ignored_exe = true;
          }
          if (item.event == soeasy::worker::ArchiveItemEvent::kSkippedTooLarge) {
            saw_skipped_large = true;
          }
          if (item.event == soeasy::worker::ArchiveItemEvent::kData && item.raw_data != nullptr) {
            std::string body(item.raw_data, item.raw_size);
            if (body.find(c.expected_substr) != std::string::npos) {
              found_expected = true;
            }
          }
          return true;
        },
        &err))
        << "Failed for " << c.file << ": " << err;
    EXPECT_TRUE(found_expected) << "Missing payload in " << c.file;
    EXPECT_FALSE(saw_ignored_exe) << "Selected filter leaked .exe in " << c.file;
    EXPECT_EQ(saw_skipped_large, c.expect_skipped_large) << "Large skip mismatch in " << c.file;
  }
}

TEST_F(ArchiveReaderIntegrationTest, DetectsEncryptedZipEntryCleanly) {
  const std::filesystem::path src = root_ / "secret.txt";
  WriteBinaryFile(src, "TopSecretPayload");
  const std::filesystem::path enc_zip = root_ / "encrypted.zip";
  const std::string py = R"PY(
import subprocess
subprocess.check_call(
    ["7z", "a", "-y", "-tzip", "-pSoEasyPass123", r")PY" +
                         enc_zip.generic_string() + R"PY(", r")PY" + src.generic_string() +
                         R"PY("],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
)PY";
  ASSERT_EQ(RunPythonScript(py), "");

  soeasy::worker::ArchiveReader reader;
  soeasy::worker::ArchiveExtractOptions opts;
  opts.enabled_exts = {"txt"};

  bool saw_encrypted = false;
  std::string err;
  reader.Extract(
      enc_zip.string(), opts,
      [&](const soeasy::worker::ArchiveItem &item) {
        if (item.event == soeasy::worker::ArchiveItemEvent::kEncrypted) {
          saw_encrypted = true;
        }
        return true;
      },
      &err);
  EXPECT_TRUE(saw_encrypted);
}

TEST_F(ArchiveReaderIntegrationTest, WorkerGuardUnpackerIsIndependentOfSingleParserPoolAndIsolatesFailures) {
  const std::string py = R"PY(
import zipfile, os
root = r""")PY" + root_.string() +
                         R"PY("""
with zipfile.ZipFile(os.path.join(root, "mixed.zip"), "w") as zf:
    zf.writestr("bad.pdf", b"%PDF-1.4 CORRUPTED_GARBAGE_WITHOUT_XREF_OR_EOF")
    zf.writestr("TEST_TRIGGER_CRASH_leaf.txt", b"Triggers SIGSEGV in parser worker")
    zf.writestr("good.txt", b"GoodArchiveSiblingToken")
)PY";
  ASSERT_EQ(RunPythonScript(py), "");

  soeasy::service::WorkerGuard guard;
  ASSERT_TRUE(guard.Init(soeasy::test::FindWorkerBin(), /*worker_count=*/1, /*timeout_ms=*/5000));

  soeasy::service::ArchiveExtractTask task;
  task.archive_path = (root_ / "mixed.zip").string();
  task.enabled_exts = {"pdf", "txt"};
  task.max_entry_bytes = 1024 * 1024;
  task.idle_heartbeat_timeout_ms = 5000;

  bool parsed_good = false;
  bool saw_bad_pdf_failure = false;
  bool saw_leaf_crash_isolated = false;
  soeasy::service::ArchiveExtractResult extract_res;
  ASSERT_TRUE(guard.ExecuteArchiveStream(
      task,
      [&](soeasy::service::ArchiveStreamItem &&item) {
        EXPECT_EQ(guard.worker_count(), 1u);
        if (item.event != soeasy::service::ArchiveStreamEvent::kData) {
          return true;
        }
        soeasy::service::ParseTask parse_task;
        parse_task.doc_id =
            (item.ext == "pdf") ? 101 : ((item.file_name.find("CRASH") != std::string::npos) ? 103 : 102);
        parse_task.file_path = item.virtual_path;
        parse_task.ext = item.ext;
        parse_task.parse_level = 1;
        parse_task.timeout_ms = 5000;
        parse_task.raw_bytes = std::make_shared<const std::string>(std::move(item.raw_bytes));

        soeasy::service::ParseTaskResult parse_res;
        EXPECT_TRUE(guard.ExecuteTask(parse_task, &parse_res));
        if (item.ext == "pdf" && parse_res.status != soeasy::service::TaskStatus::kSuccess) {
          saw_bad_pdf_failure = true;
        }
        if (item.file_name.find("TEST_TRIGGER_CRASH") != std::string::npos &&
            parse_res.status == soeasy::service::TaskStatus::kCrashed) {
          saw_leaf_crash_isolated = true;
        }
        if (item.file_name == "good.txt" && parse_res.status == soeasy::service::TaskStatus::kSuccess &&
            parse_res.extracted_text.find("GoodArchiveSiblingToken") != std::string::npos) {
          parsed_good = true;
        }
        return true;
      },
      &extract_res));

  EXPECT_EQ(extract_res.status, soeasy::service::TaskStatus::kSuccess) << "msg=" << extract_res.error_message;
  EXPECT_TRUE(saw_bad_pdf_failure);
  EXPECT_TRUE(saw_leaf_crash_isolated);
  EXPECT_TRUE(parsed_good);
  guard.Shutdown();
}

TEST_F(ArchiveReaderIntegrationTest, EndToEndOrchestratorLifecycleSearchUpdateDeleteAndConfigPrune) {
  const std::filesystem::path docs_dir = root_ / "docs";
  const std::filesystem::path db_path = root_ / "archive_e2e.db";
  std::filesystem::create_directories(docs_dir);

  const std::string py_create = R"PY(
import tarfile, zipfile, os
docs = r""")PY" + docs_dir.string() +
                                R"PY("""
with tarfile.open(os.path.join(docs, "_inner.tar.gz"), "w:gz") as tf:
    p = os.path.join(docs, "_leaf.txt")
    with open(p, "wb") as f:
        f.write(b"AlphaArchiveDeepLeafToken_2026 nested archive body\n")
    tf.add(p, arcname="sub/contract_alpha.txt")
    os.remove(p)

with zipfile.ZipFile(os.path.join(docs, "bundle.zip"), "w", zipfile.ZIP_DEFLATED) as zf:
    zf.write(os.path.join(docs, "_inner.tar.gz"), arcname="nested/inner.tar.gz")
    zf.writestr("top_notes.md", b"TopMarkdownArchiveToken\n")
    zf.writestr("oversized_leaf.txt", b"OversizedArchiveLeafShouldBeSkipped " * 40)
    zf.writestr("padding.bin", os.urandom(2048))
os.remove(os.path.join(docs, "_inner.tar.gz"))
)PY";
  ASSERT_EQ(RunPythonScript(py_create), "");
  ASSERT_GT(std::filesystem::file_size(docs_dir / "bundle.zip"), 1500u);

  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  ASSERT_TRUE(storage->Open(db_path.string(), "SoEasy_Archive_E2E_Pass!", soeasy::test::FindDictDir()));

  auto guard = std::make_shared<soeasy::service::WorkerGuard>();
  ASSERT_TRUE(guard->Init(soeasy::test::FindWorkerBin(), 2, 5000));

  auto config = std::make_shared<soeasy::service::IndexConfig>();
  config->SetIndexedDirectories({docs_dir.string()});
  // Container file (~2KB+) exceeds max_file_size (600B) and MUST still be traversed;
  // oversized_leaf.txt (>600B) MUST be skipped; contract_alpha.txt (~50B) MUST be indexed.
  config->set_max_file_size(600);

  auto exts_set = config->GetSupportedExtensions();
  for (const auto &aext : soeasy::common::GetSupportedArchiveExtensions()) {
    exts_set.insert(aext);
  }
  std::vector<std::string> enabled_exts(exts_set.begin(), exts_set.end());
  config->SetSupportedExtensions(enabled_exts);

  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(config, guard, storage);
  ASSERT_TRUE(pipeline->Init());

  pipeline->PerformInitialScan();
  pipeline->FlushWriteQueue();

  // 1. Fulltext search hits nested leaf and returns host container_path; oversized leaf is NOT content-indexed!
  const auto ft_hits = pipeline->SearchFulltext("AlphaArchiveDeepLeafToken_2026", 10, 0);
  ASSERT_EQ(ft_hits.size(), 1u);
  EXPECT_EQ(ft_hits[0].file_name, "contract_alpha.txt");
  EXPECT_EQ(ft_hits[0].container_path, (docs_dir / "bundle.zip").generic_string());
  EXPECT_TRUE(pipeline->SearchFulltext("OversizedArchiveLeafShouldBeSkipped", 10, 0).empty());
  EXPECT_EQ(storage->GetParseStatusSummary().failed_count, 0u);
  EXPECT_TRUE(storage->GetParseIssues().empty());

  // 2. Filename search hits both leaf (with container_path) and container bundle.zip itself (empty container_path)!
  const auto leaf_name_hits = pipeline->SearchFilename("contract_alpha", 10, 0);
  ASSERT_EQ(leaf_name_hits.size(), 1u);
  EXPECT_EQ(leaf_name_hits[0].container_path, (docs_dir / "bundle.zip").generic_string());

  const auto hybrid_hits = pipeline->search_coordinator()->SearchHybrid("AlphaArchiveDeepLeafToken_2026", 10, 0);
  ASSERT_EQ(hybrid_hits.size(), 1u);
  EXPECT_EQ(hybrid_hits[0].container_path, (docs_dir / "bundle.zip").generic_string());

  const auto zip_name_hits = pipeline->SearchFilename("bundle.zip", 10, 0);
  ASSERT_EQ(zip_name_hits.size(), 1u);
  EXPECT_TRUE(zip_name_hits[0].container_path.empty());

  // 3. Re-running PerformInitialScan invokes SweepOfflineDeletedFiles and MUST preserve archive children!
  pipeline->PerformInitialScan();
  pipeline->FlushWriteQueue();
  EXPECT_EQ(pipeline->SearchFulltext("AlphaArchiveDeepLeafToken_2026", 10, 0).size(), 1u);

  // 4. Modify bundle.zip: remove contract_alpha.txt & add contract_beta.txt -> re-index cleans stale child!
  const std::string py_update = R"PY(
import zipfile, os
docs = r""")PY" + docs_dir.string() +
                                R"PY("""
with zipfile.ZipFile(os.path.join(docs, "bundle.zip"), "w", zipfile.ZIP_DEFLATED) as zf:
    zf.writestr("contract_beta.txt", b"BetaUpdatedArchiveLeafToken_2026")
)PY";
  ASSERT_EQ(RunPythonScript(py_update), "");
  std::filesystem::last_write_time(docs_dir / "bundle.zip",
                                   std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2));

  pipeline->PerformInitialScan();
  pipeline->FlushWriteQueue();
  EXPECT_TRUE(pipeline->SearchFulltext("AlphaArchiveDeepLeafToken_2026", 10, 0).empty());
  const auto beta_hits = pipeline->SearchFulltext("BetaUpdatedArchiveLeafToken_2026", 10, 0);
  ASSERT_EQ(beta_hits.size(), 1u);
  EXPECT_EQ(beta_hits[0].container_path, (docs_dir / "bundle.zip").generic_string());

  // 5. Disable archive extensions and prune -> removes both bundle.zip and its children!
  for (const auto &aext : soeasy::common::GetSupportedArchiveExtensions()) {
    exts_set.erase(aext);
  }
  config->SetSupportedExtensions(std::vector<std::string>(exts_set.begin(), exts_set.end()));
  const auto stats_before_prune = pipeline->GetStats();
  const size_t pruned_count = pipeline->PruneStaleEntriesByConfig();
  EXPECT_GE(pruned_count, 2u);
  EXPECT_EQ(pipeline->GetStats().total_files_deleted, stats_before_prune.total_files_deleted + pruned_count);
  EXPECT_TRUE(pipeline->SearchFulltext("BetaUpdatedArchiveLeafToken_2026", 10, 0).empty());
  EXPECT_TRUE(pipeline->SearchFilename("bundle.zip", 10, 0).empty());
  EXPECT_TRUE(storage->ListArchiveContainers().empty());

  guard->Shutdown();
  storage->Close();
}

TEST_F(ArchiveReaderIntegrationTest, MigratesLegacyFilesTableMissingParentId) {
  const std::filesystem::path work_dir = soeasy::test::GetTestTempPath("legacy_schema_upgrade");
  std::filesystem::remove_all(work_dir);
  std::filesystem::create_directories(work_dir);
  const std::filesystem::path db_path = work_dir / "legacy.db";

  const std::string py =
      "import sqlite3\n"
      "conn = sqlite3.connect(r'" +
      db_path.string() +
      "')\n"
      "conn.executescript('''\n"
      "  CREATE TABLE files (\n"
      "    doc_id INTEGER PRIMARY KEY,\n"
      "    file_name TEXT NOT NULL,\n"
      "    file_path TEXT NOT NULL UNIQUE,\n"
      "    ext TEXT NOT NULL,\n"
      "    file_size INTEGER NOT NULL DEFAULT 0,\n"
      "    mtime INTEGER NOT NULL DEFAULT 0,\n"
      "    parse_level INTEGER NOT NULL DEFAULT 0,\n"
      "    parse_stat INTEGER NOT NULL DEFAULT 0\n"
      "  );\n"
      "  CREATE TABLE doc_contents (doc_id INTEGER PRIMARY KEY, raw_size INTEGER NOT NULL, "
      "summary TEXT, zstd_body BLOB NOT NULL);\n"
      "  CREATE TABLE ext_stats (ext TEXT PRIMARY KEY NOT NULL, active_count INTEGER NOT NULL DEFAULT 0);\n"
      "  CREATE TABLE index_meta (key TEXT PRIMARY KEY NOT NULL, value TEXT NOT NULL);\n"
      "  INSERT INTO files (doc_id, file_name, file_path, ext, file_size, mtime, parse_level, parse_stat) "
      "VALUES (42, 'old.txt', '/tmp/old.txt', 'txt', 10, 100, 1, 1);\n"
      "''')\n"
      "conn.commit()\n"
      "conn.close()\n";
  ASSERT_EQ(RunPythonScript(py), "");

  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  ASSERT_TRUE(storage->Open(db_path.string(), "", soeasy::test::FindDictDir())) << storage->GetLastError();

  soeasy::storage::ArchiveContainerMeta meta;
  meta.container_id = 7001;
  meta.container_path = "/tmp/migrated.zip";
  meta.file_size = 512;
  meta.mtime = 1000;
  meta.status = static_cast<int>(soeasy::common::ParseStatus::kOk);
  meta.item_count = 1;
  ASSERT_TRUE(storage->UpsertArchiveContainer(meta));

  soeasy::storage::DocumentIndexEntry child;
  child.doc_id = 7002;
  child.parent_id = 7001;
  child.file_path = "/tmp/migrated.zip!/inside.txt";
  child.file_name = "inside.txt";
  child.ext = "txt";
  child.file_size = 24;
  child.mtime = 1000;
  child.parse_level = 1;
  child.utf8_content = "LegacyMigrationArchiveToken_2026";
  child.parse_stat = static_cast<int>(soeasy::common::ParseStatus::kOk);
  ASSERT_TRUE(storage->IndexDocument(child));

  const auto parent_map = storage->GetContainerPathsByDocIds({7002, 42});
  ASSERT_EQ(parent_map.size(), 1u);
  EXPECT_EQ(parent_map.at(7002), "/tmp/migrated.zip");

  storage->Close();
  std::filesystem::remove_all(work_dir);
}

TEST_F(ArchiveReaderIntegrationTest, ReindexesInterruptedArchiveWithUnknownStatus) {
  const std::filesystem::path work_dir = soeasy::test::GetTestTempPath("cancelled_archive_rescan");
  std::filesystem::remove_all(work_dir);
  const std::filesystem::path docs_dir = work_dir / "docs";
  std::filesystem::create_directories(docs_dir);

  const std::string py =
      "import os, zipfile\n"
      "docs = r'" +
      docs_dir.string() +
      "'\n"
      "with zipfile.ZipFile(os.path.join(docs, 'partial.zip'), 'w', zipfile.ZIP_DEFLATED) as zf:\n"
      "    zf.writestr('recovered.txt', b'RecoveredCancelledArchiveToken_2026')\n";
  ASSERT_EQ(RunPythonScript(py), "");

  const std::string db_path = (work_dir / "index.db").string();
  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  ASSERT_TRUE(storage->Open(db_path, "", soeasy::test::FindDictDir())) << storage->GetLastError();

  auto config = std::make_shared<soeasy::service::IndexConfig>();
  config->SetIndexedDirectories({docs_dir.string()});
  config->SetSupportedExtensions({"txt", "zip"});

  auto guard = std::make_shared<soeasy::service::WorkerGuard>();
  ASSERT_TRUE(guard->Init(soeasy::test::FindWorkerBin(), 1, 15000));

  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(config, guard, storage);
  ASSERT_TRUE(pipeline->Init());

  // Seed container row with kUnknown (0) at matching size/mtime, simulating an aborted mid-archive scan.
  const std::filesystem::path archive_path = docs_dir / "partial.zip";
  uint64_t fsize = 0;
  int64_t fmtime = 0;
  ASSERT_TRUE(soeasy::common::EncodingUtils::GetFileStat(archive_path.string(), &fsize, &fmtime));
  soeasy::storage::ArchiveContainerMeta partial_meta;
  partial_meta.container_id = soeasy::service::IndexPipeline::GenerateDocId(archive_path.generic_string());
  partial_meta.container_path = archive_path.generic_string();
  partial_meta.file_size = fsize;
  partial_meta.mtime = fmtime;
  partial_meta.status = static_cast<int>(soeasy::common::ParseStatus::kUnknown);
  partial_meta.item_count = 0;
  ASSERT_TRUE(storage->UpsertArchiveContainer(partial_meta));

  pipeline->PerformInitialScan();
  pipeline->FlushWriteQueue();

  const auto hits = pipeline->SearchFulltext("RecoveredCancelledArchiveToken_2026", 10, 0);
  ASSERT_EQ(hits.size(), 1u);
  soeasy::storage::ArchiveContainerMeta updated_meta;
  ASSERT_TRUE(storage->GetArchiveContainerMeta(partial_meta.container_id, &updated_meta));
  EXPECT_EQ(updated_meta.status, static_cast<int>(soeasy::common::ParseStatus::kOk));
  EXPECT_EQ(updated_meta.item_count, 1);

  guard->Shutdown();
  storage->Close();
  std::filesystem::remove_all(work_dir);
}

TEST_F(ArchiveReaderIntegrationTest, StreamsArchiveLeafLargerThan64MiBWithoutError) {
  const std::filesystem::path work_dir = soeasy::test::GetTestTempPath("chunked_over_64mib");
  std::filesystem::remove_all(work_dir);
  const std::filesystem::path docs_dir = work_dir / "docs";
  std::filesystem::create_directories(docs_dir);

  const std::string py =
      "import os, zipfile\n"
      "docs = r'" +
      docs_dir.string() +
      "'\n"
      "huge_html = b'<div data=\"' + (b'A' * (68 * 1024 * 1024)) + "
      "b'\">Over64MiBChunkedArchiveLeafToken_2026</div>\\n'\n"
      "too_big_html = b'<div data=\"' + (b'B' * (85 * 1024 * 1024)) + b'\">TooBigLeafShouldBeSkippedToken</div>\\n'\n"
      "with zipfile.ZipFile(os.path.join(docs, 'large_leaves.zip'), 'w', zipfile.ZIP_DEFLATED) as zf:\n"
      "    zf.writestr('big_ok.html', huge_html)\n"
      "    zf.writestr('too_big.html', too_big_html)\n";
  ASSERT_EQ(RunPythonScript(py), "");

  const std::string db_path = (work_dir / "index.db").string();
  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  ASSERT_TRUE(storage->Open(db_path, "", soeasy::test::FindDictDir())) << storage->GetLastError();

  auto config = std::make_shared<soeasy::service::IndexConfig>();
  config->SetIndexedDirectories({docs_dir.string()});
  config->SetSupportedExtensions({"html", "txt", "zip"});
  config->set_max_file_size(80ULL * 1024ULL * 1024ULL);

  auto guard = std::make_shared<soeasy::service::WorkerGuard>();
  ASSERT_TRUE(guard->Init(soeasy::test::FindWorkerBin(), 1, 30000));

  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(config, guard, storage);
  ASSERT_TRUE(pipeline->Init());

  pipeline->PerformInitialScan();
  pipeline->FlushWriteQueue();

  const auto hits = pipeline->SearchFulltext("Over64MiBChunkedArchiveLeafToken_2026", 10, 0);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].file_name, "big_ok.html");
  EXPECT_GT(hits[0].file_size, 64ULL * 1024ULL * 1024ULL);
  EXPECT_TRUE(pipeline->SearchFulltext("TooBigLeafShouldBeSkippedToken", 10, 0).empty());
  EXPECT_EQ(storage->GetParseStatusSummary().failed_count, 0u);
  EXPECT_TRUE(storage->GetParseIssues().empty());

  // Also verify > 64 MiB extracted text returning from Worker via chunked TextStreamReceiver.
  auto big_txt = std::make_shared<std::string>(66ULL * 1024ULL * 1024ULL, 'C');
  soeasy::service::ParseTask big_task{99001, "/virtual/big_out.txt", "txt", 1, 30000, big_txt};
  soeasy::service::ParseTaskResult big_res;
  ASSERT_TRUE(guard->ExecuteTask(big_task, &big_res));
  EXPECT_EQ(big_res.status, soeasy::service::TaskStatus::kSuccess) << big_res.error_message;
  EXPECT_EQ(big_res.extracted_text.size(), 66ULL * 1024ULL * 1024ULL);
  EXPECT_GT(big_res.stream_chunk_count, 256u);

  guard->Shutdown();
  storage->Close();
  std::filesystem::remove_all(work_dir);
}

TEST_F(ArchiveReaderIntegrationTest, InspectsRealTestDatasetIfPresent) {
  const std::filesystem::path dataset_dir = "test_dataset";
  if (!std::filesystem::exists(dataset_dir) || !std::filesystem::is_directory(dataset_dir)) {
    GTEST_SKIP() << "test_dataset directory not present";
  }

  soeasy::worker::ArchiveReader reader;
  soeasy::worker::ArchiveExtractOptions opts;
  opts.enabled_exts = {"txt", "md", "doc", "docx", "pdf", "xlsx", "pptx"};

  size_t found_archives = 0;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(dataset_dir)) {
    if (!entry.is_regular_file()) continue;
    std::string ext = entry.path().extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (!soeasy::common::IsSupportedArchiveExtension(ext)) continue;
    ++found_archives;
    reader.Extract(entry.path().string(), opts, [](const soeasy::worker::ArchiveItem &) { return true; });
  }
  if (found_archives == 0) {
    GTEST_SKIP() << "No archive fixtures inside test_dataset";
  }
}

}  // namespace
