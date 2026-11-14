#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/name_index.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"

namespace soeasy {
namespace service {
namespace {

void WriteFile(const std::filesystem::path &path, const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

bool WaitUntil(const std::function<bool()> &pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return pred();
}

class IndexStateMachineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto stamp = std::to_string(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    root_ = soeasy::test::GetTestTempPath("soeasy_sm_test_" + stamp);
    dir_a_ = root_ / "dir_a";
    dir_b_ = root_ / "dir_b";
    outside_dir_ = root_ / "outside";
    db_path_ = root_ / "test.db";

    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(dir_a_);
    std::filesystem::create_directories(dir_b_);
    std::filesystem::create_directories(outside_dir_);
  }

  void TearDown() override {
    if (pipeline_) {
      pipeline_->StopLiveWatch();
      pipeline_.reset();
    }
    worker_guard_.reset();
    if (storage_) {
      storage_->Close();
      storage_.reset();
    }
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  void InitPipeline(uint32_t debounce_ms = 60) {
    storage_ = std::make_shared<storage::StorageEngine>();
    ASSERT_TRUE(storage_->Open(db_path_.string(), "SoEasy_SM_Test_Passphrase!", soeasy::test::FindDictDir()));

    worker_guard_ = std::make_shared<WorkerGuard>();
    ASSERT_TRUE(worker_guard_->Init(soeasy::test::FindWorkerBin(), 2, 5000, 4, 4 * 1024 * 1024));

    config_ = std::make_shared<IndexConfig>();
    config_->set_debounce_ms(debounce_ms);

    pipeline_ = std::make_shared<IndexPipeline>(config_, worker_guard_, storage_);
    ASSERT_TRUE(pipeline_->Init());
  }

  std::filesystem::path root_;
  std::filesystem::path dir_a_;
  std::filesystem::path dir_b_;
  std::filesystem::path outside_dir_;
  std::filesystem::path db_path_;

  std::shared_ptr<storage::StorageEngine> storage_;
  std::shared_ptr<WorkerGuard> worker_guard_;
  std::shared_ptr<IndexConfig> config_;
  std::shared_ptr<IndexPipeline> pipeline_;
};

TEST_F(IndexStateMachineTest, DiffIndexConfigDetectsAllTransitions) {
  IndexConfig cfg;
  cfg.SetIndexedDirectories({"/tmp/dir_a", "/tmp/dir_b/"});
  cfg.SetExcludedPatterns({".git", "node_modules"});
  cfg.SetSupportedExtensions({"txt", "md", "cpp"});
  cfg.set_max_file_size(10 * 1024 * 1024);
  cfg.set_filter_cache_thumbnails(true);
  cfg.set_debounce_ms(300);
  cfg.set_ui_language("zh");

  const IndexConfigSnapshot before = cfg.CaptureSnapshot();

  // Trailing slash normalization & reordering should not register structural changes.
  cfg.SetIndexedDirectories({"/tmp/dir_b", "/tmp/dir_a/"});
  {
    const auto d = DiffIndexConfig(before, cfg.CaptureSnapshot());
    EXPECT_FALSE(d.HasStructuralChanges());
    EXPECT_FALSE(d.HasAnyChanges());
  }

  // Runtime-only changes.
  cfg.set_ui_language("en");
  cfg.set_debounce_ms(120);
  {
    const auto d = DiffIndexConfig(before, cfg.CaptureSnapshot());
    EXPECT_TRUE(d.runtime_changed);
    EXPECT_TRUE(d.debounce_changed);
    EXPECT_FALSE(d.HasStructuralChanges());
  }

  // Directory add + remove, extension narrow.
  const IndexConfigSnapshot base2 = cfg.CaptureSnapshot();
  cfg.SetIndexedDirectories({"/tmp/dir_a", "/tmp/dir_c"});
  cfg.SetSupportedExtensions({"txt", "cpp"});
  {
    const auto d = DiffIndexConfig(base2, cfg.CaptureSnapshot());
    EXPECT_TRUE(d.HasStructuralChanges());
    ASSERT_EQ(d.removed_dirs.size(), 1u);
    EXPECT_EQ(d.removed_dirs[0], "/tmp/dir_b");
    ASSERT_EQ(d.added_dirs.size(), 1u);
    EXPECT_EQ(d.added_dirs[0], "/tmp/dir_c");
    EXPECT_TRUE(d.rules_narrowed);
    EXPECT_FALSE(d.rules_expanded);
  }

  // Adding exclusion shrinks size, disabling thumbnail filter expands rules, toggling OCR expands rules.
  const IndexConfigSnapshot base3 = cfg.CaptureSnapshot();
  cfg.AddExcludedPattern(".cache");
  {
    const auto d = DiffIndexConfig(base3, cfg.CaptureSnapshot());
    EXPECT_TRUE(d.rules_narrowed);
  }

  const IndexConfigSnapshot base4 = cfg.CaptureSnapshot();
  cfg.set_filter_cache_thumbnails(false);
  {
    const auto d = DiffIndexConfig(base4, cfg.CaptureSnapshot());
    EXPECT_TRUE(d.rules_expanded);
    EXPECT_FALSE(d.ocr_expanded);
  }

  const IndexConfigSnapshot base5 = cfg.CaptureSnapshot();
  cfg.set_enable_embedded_image_ocr(true);
  {
    const auto d = DiffIndexConfig(base5, cfg.CaptureSnapshot());
    EXPECT_TRUE(d.rules_expanded);
    EXPECT_TRUE(d.ocr_expanded);
  }
}

TEST_F(IndexStateMachineTest, IdleNarrowingDirectoryRemovalAndOfflineMarkAndSweep) {
  InitPipeline();

  WriteFile(dir_a_ / "keep.txt", "alpha_keep_token");
  WriteFile(dir_a_ / "prune.md", "markdown_prune_token");
  WriteFile(dir_a_ / "offline_victim.txt", "offline_victim_token");
  WriteFile(dir_b_ / "other_dir.txt", "dir_b_removed_token");

  config_->SetIndexedDirectories({dir_a_.string(), dir_b_.string()});
  config_->SetSupportedExtensions({"txt", "md"});
  ASSERT_EQ(pipeline_->PerformInitialScan(), 4u);
  EXPECT_EQ(pipeline_->IndexedFileCount(), 4u);

  // 1. Narrow extension (.md unchecked) and remove dir_b via ApplyConfigChanges.
  const IndexConfigSnapshot before = config_->CaptureSnapshot();
  config_->SetIndexedDirectories({dir_a_.string()});
  config_->SetSupportedExtensions({"txt"});
  const ConfigChangeSummary diff = DiffIndexConfig(before, config_->CaptureSnapshot());
  ASSERT_TRUE(diff.rules_narrowed);
  ASSERT_FALSE(diff.rules_expanded);
  ASSERT_EQ(diff.removed_dirs.size(), 1u);

  std::mutex fin_mu;
  std::condition_variable fin_cv;
  bool finished = false;
  BackgroundIndexCallbacks cb;
  cb.on_finished = [&](const IndexTaskResult &) {
    std::lock_guard<std::mutex> lk(fin_mu);
    finished = true;
    fin_cv.notify_one();
  };
  pipeline_->ApplyConfigChanges(diff, cb);
  {
    std::unique_lock<std::mutex> lk(fin_mu);
    ASSERT_TRUE(fin_cv.wait_for(lk, std::chrono::seconds(8), [&]() { return finished; }));
  }

  EXPECT_EQ(pipeline_->IndexedFileCount(), 2u);
  EXPECT_TRUE(pipeline_->SearchFilename("keep.txt", 10).size() == 1);
  EXPECT_TRUE(pipeline_->SearchFilename("prune.md", 10).empty());
  EXPECT_TRUE(pipeline_->SearchFilename("other_dir.txt", 10).empty());
  EXPECT_TRUE(pipeline_->SearchFulltext("markdown_prune_token", 10, 0).empty());
  EXPECT_TRUE(pipeline_->SearchFulltext("dir_b_removed_token", 10, 0).empty());

  // 2. Delete offline_victim.txt on disk while watch is stopped, then run PerformInitialScan Mark-and-Sweep.
  std::filesystem::remove(dir_a_ / "offline_victim.txt");
  pipeline_->PerformInitialScan();
  EXPECT_EQ(pipeline_->IndexedFileCount(), 1u);
  EXPECT_TRUE(pipeline_->SearchFilename("offline_victim.txt", 10).empty());
  EXPECT_TRUE(pipeline_->SearchFulltext("offline_victim_token", 10, 0).empty());
  EXPECT_EQ(pipeline_->SearchFilename("keep.txt", 10).size(), 1u);
}

TEST_F(IndexStateMachineTest, WatchExtensionlessDirectoryMoveExcludeAndDelete) {
  InitPipeline(/*debounce_ms=*/60);

  std::filesystem::create_directories(dir_a_ / "node_modules");
  WriteFile(dir_a_ / "stay.txt", "stay_token");
  WriteFile(dir_a_ / "sub" / "nested.txt", "nested_alpha_token");
  WriteFile(outside_dir_ / "fresh.txt", "moved_in_fresh_token");

  config_->SetIndexedDirectories({dir_a_.string()});
  config_->SetSupportedExtensions({"txt"});
  ASSERT_EQ(pipeline_->PerformInitialScan(), 2u);
  ASSERT_TRUE(pipeline_->StartLiveWatch());

  // Move extensionless sub/ -> sub_renamed/ inside watched dir.
  std::filesystem::rename(dir_a_ / "sub", dir_a_ / "sub_renamed");
  ASSERT_TRUE(WaitUntil([&]() {
    auto old_hits = pipeline_->SearchFilename("nested.txt", 10);
    bool has_new = false;
    bool has_old = false;
    for (const auto &h : old_hits) {
      if (h.file_path.find("sub_renamed") != std::string::npos) has_new = true;
      if (h.file_path.find("/sub/") != std::string::npos) has_old = true;
    }
    return has_new && !has_old;
  }));

  // Move sub_renamed/ into excluded folder node_modules/sub_renamed -> old subtree must be deleted.
  std::filesystem::rename(dir_a_ / "sub_renamed", dir_a_ / "node_modules" / "sub_renamed");
  ASSERT_TRUE(WaitUntil([&]() { return pipeline_->SearchFilename("nested.txt", 10).empty(); }));
  EXPECT_TRUE(pipeline_->SearchFulltext("nested_alpha_token", 10, 0).empty());

  // Move pre-populated directory from outside_dir_ into dir_a_/incoming -> files inside must be indexed.
  std::filesystem::rename(outside_dir_, dir_a_ / "incoming");
  ASSERT_TRUE(WaitUntil([&]() { return pipeline_->SearchFilename("fresh.txt", 10).size() == 1; }));
  ASSERT_TRUE(WaitUntil([&]() { return pipeline_->SearchFulltext("moved_in_fresh_token", 10, 0).size() == 1; }));

  // Delete entire extensionless directory incoming/ -> files inside must be removed.
  std::filesystem::remove_all(dir_a_ / "incoming");
  ASSERT_TRUE(WaitUntil([&]() { return pipeline_->SearchFilename("fresh.txt", 10).empty(); }));
  EXPECT_TRUE(pipeline_->SearchFulltext("moved_in_fresh_token", 10, 0).empty());
  EXPECT_EQ(pipeline_->SearchFilename("stay.txt", 10).size(), 1u);
}

TEST_F(IndexStateMachineTest, RebuildWatchFirstReconcileNoDowngradeAndIncompleteMeta) {
  InitPipeline(/*debounce_ms=*/60);

  WriteFile(dir_a_ / "keep.txt", "initial_keep_token");
  WriteFile(dir_a_ / "modify_me.txt", "before_reconcile_token");
  WriteFile(dir_a_ / "delete_me.txt", "delete_during_rebuild_token");
  for (int i = 0; i < 12; ++i) {
    WriteFile(dir_a_ / ("bulk_" + std::to_string(i) + ".txt"), "bulk_payload_" + std::to_string(i));
  }

  config_->SetIndexedDirectories({dir_a_.string()});
  config_->SetSupportedExtensions({"txt"});
  ASSERT_EQ(pipeline_->PerformInitialScan(), 15u);
  ASSERT_TRUE(pipeline_->StartLiveWatch());

  // Part 1: RebuildIndex with live Watch-First + buffered edits reconciled even within same-second mtime.
  std::atomic<bool> injected_edits{false};
  const bool rebuild_ok = pipeline_->RebuildIndex([&](size_t, size_t, const std::string &) {
    if (!injected_edits.exchange(true)) {
      WriteFile(dir_a_ / "modify_me.txt", "RECONCILE_ALPHA_TOKEN");
      WriteFile(dir_a_ / "created_during_rebuild.txt", "RECONCILE_BETA_TOKEN");
      std::filesystem::remove(dir_a_ / "delete_me.txt");
      // Give efsw + debouncer enough time to deliver events into buffered_watch_events_ while RebuildIndex runs.
      std::this_thread::sleep_for(std::chrono::milliseconds(220));
    }
  });
  ASSERT_TRUE(rebuild_ok);

  EXPECT_TRUE(pipeline_->SearchFulltext("RECONCILE_ALPHA_TOKEN", 10, 0).size() == 1);
  EXPECT_TRUE(pipeline_->SearchFulltext("RECONCILE_BETA_TOKEN", 10, 0).size() == 1);
  EXPECT_TRUE(pipeline_->SearchFilename("delete_me.txt", 10).empty());
  EXPECT_TRUE(pipeline_->SearchFulltext("delete_during_rebuild_token", 10, 0).empty());

  // Part 2: kRebuild is never cancelled or downgraded by kIncremental / runtime ApplyConfigChanges,
  // and cancelled kRebuild sets HasIncompleteRebuild() + DB meta until recovery completes.
  std::mutex mu;
  std::condition_variable cv;
  std::vector<IndexTaskResult> results;
  std::atomic<bool> in_progress{false};
  std::atomic<bool> slow_progress_once{false};

  BackgroundIndexCallbacks cb;
  cb.on_progress = [&](IndexTaskKind kind, uint64_t, size_t, size_t, const std::string &) {
    if (kind == IndexTaskKind::kRebuild) {
      in_progress.store(true);
      if (!slow_progress_once.exchange(true)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
      }
    }
  };
  cb.on_finished = [&](const IndexTaskResult &res) {
    std::lock_guard<std::mutex> lk(mu);
    results.push_back(res);
    cv.notify_all();
  };

  pipeline_->StartBackgroundTask(/*rebuild=*/true, /*force_incremental=*/false, cb);
  ASSERT_TRUE(WaitUntil([&]() { return in_progress.load(); }));

  // Attempt to issue incremental scan and runtime config apply while rebuild is active:
  // active rebuild must NOT be cancelled or downgraded.
  pipeline_->StartBackgroundTask(/*rebuild=*/false, /*force_incremental=*/true, cb);
  {
    const auto before = config_->CaptureSnapshot();
    config_->set_ui_language("en");
    pipeline_->ApplyConfigChanges(DiffIndexConfig(before, config_->CaptureSnapshot()), cb);
  }

  {
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(10), [&]() { return !results.empty(); }));
  }
  ASSERT_EQ(results[0].kind, IndexTaskKind::kRebuild);
  EXPECT_FALSE(results[0].cancelled);
  EXPECT_TRUE(results[0].success);

  // Wait for any follow-up incremental task to drain.
  ASSERT_TRUE(WaitUntil([&]() { return !pipeline_->IsIndexing(); }));

  // Part 3: Cancel an active kRebuild -> HasIncompleteRebuild() & DB meta must be true; full recovery clears it.
  {
    std::lock_guard<std::mutex> lk(mu);
    results.clear();
  }
  in_progress.store(false);
  slow_progress_once.store(false);

  BackgroundIndexCallbacks cancel_cb;
  cancel_cb.on_progress = [&](IndexTaskKind kind, uint64_t, size_t, size_t, const std::string &) {
    if (kind == IndexTaskKind::kRebuild) {
      in_progress.store(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
  };
  cancel_cb.on_finished = [&](const IndexTaskResult &res) {
    std::lock_guard<std::mutex> lk(mu);
    results.push_back(res);
    cv.notify_all();
  };

  pipeline_->StartBackgroundTask(/*rebuild=*/true, /*force_incremental=*/false, cancel_cb);
  ASSERT_TRUE(WaitUntil([&]() { return in_progress.load(); }));
  pipeline_->CancelScan();

  {
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(10), [&]() { return !results.empty(); }));
  }
  EXPECT_TRUE(results.back().cancelled);
  EXPECT_TRUE(pipeline_->HasIncompleteRebuild());
  EXPECT_EQ(pipeline_->GetPersistedIndexState(), PersistedIndexState::kRebuildingIncomplete);

  std::string meta_val;
  ASSERT_TRUE(storage_->GetMetaValue("rebuild_incomplete", &meta_val));
  EXPECT_EQ(meta_val, "1");
  ASSERT_TRUE(storage_->GetMetaValue("index_state", &meta_val));
  EXPECT_EQ(meta_val, "rebuilding_incomplete");

  // Completing a clean scan resets rebuild_incomplete to 0, index_state to ready, and returns phase to kIdle.
  pipeline_->PerformInitialScan();
  EXPECT_EQ(pipeline_->GetPhase(), OrchestratorPhase::kIdle);
  EXPECT_FALSE(pipeline_->HasIncompleteRebuild());
  EXPECT_EQ(pipeline_->GetPersistedIndexState(), PersistedIndexState::kReady);
  ASSERT_TRUE(storage_->GetMetaValue("rebuild_incomplete", &meta_val));
  EXPECT_EQ(meta_val, "0");
  ASSERT_TRUE(storage_->GetMetaValue("index_state", &meta_val));
  EXPECT_EQ(meta_val, "ready");
}

TEST_F(IndexStateMachineTest, InterruptedInitialScanResumesAfterNarrowingConfigSync) {
  InitPipeline();

  for (int i = 0; i < 5; ++i) {
    WriteFile(dir_a_ / ("doc_" + std::to_string(i) + ".txt"), "batch_txt_token_" + std::to_string(i));
  }
  WriteFile(dir_a_ / "drop_me.md", "narrow_drop_md_token");
  config_->SetIndexedDirectories({dir_a_.string()});
  config_->SetSupportedExtensions({"txt", "md"});

  std::mutex mu;
  std::condition_variable cv;
  bool in_initial = false;
  bool narrow_applied = false;
  bool initial_was_cancelled = false;
  std::atomic<bool> paused_once{false};

  BackgroundIndexCallbacks cb;
  cb.on_started = [&](IndexTaskKind kind, uint64_t) {
    if (kind == IndexTaskKind::kInitial && !paused_once.exchange(true)) {
      std::unique_lock<std::mutex> lk(mu);
      in_initial = true;
      cv.notify_all();
      cv.wait_for(lk, std::chrono::seconds(3), [&]() { return narrow_applied; });
    }
  };
  cb.on_finished = [&](const IndexTaskResult &res) {
    if (res.kind == IndexTaskKind::kInitial && res.cancelled) {
      std::lock_guard<std::mutex> lk(mu);
      initial_was_cancelled = true;
    }
  };

  pipeline_->StartBackgroundTask(/*rebuild=*/false, /*force_incremental=*/false, cb);
  {
    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(5), [&]() { return in_initial; }));
  }

  const IndexConfigSnapshot before_narrow = config_->CaptureSnapshot();
  config_->SetSupportedExtensions({"txt"});  // narrowing only (removes md)
  const ConfigChangeSummary narrow_diff = DiffIndexConfig(before_narrow, config_->CaptureSnapshot());
  ASSERT_TRUE(narrow_diff.rules_narrowed);
  ASSERT_FALSE(narrow_diff.rules_expanded);
  pipeline_->ApplyConfigChanges(narrow_diff, cb);

  {
    std::lock_guard<std::mutex> lk(mu);
    narrow_applied = true;
  }
  cv.notify_all();

  ASSERT_TRUE(WaitUntil([&]() { return !pipeline_->IsIndexing(); }));
  EXPECT_TRUE(initial_was_cancelled);
  EXPECT_EQ(pipeline_->GetPhase(), OrchestratorPhase::kIdle);
  EXPECT_TRUE(pipeline_->SearchFilename("drop_me.md", 10).empty());
  EXPECT_EQ(pipeline_->IndexedFileCount(), 5u);
  EXPECT_EQ(pipeline_->SearchFilename("doc_4.txt", 10).size(), 1u);
}

TEST_F(IndexStateMachineTest, DirectSyncResetsPhaseAndOcrExpandedReindexes) {
  InitPipeline();

  // 1. Direct synchronous PerformInitialScan() and RebuildIndex() must always restore kIdle phase.
  WriteFile(dir_a_ / "sync_check.txt", "sync_phase_reset_token");
  WriteFile(dir_a_ / "ocr_candidate.pdf", "%PDF-1.4\n" + std::string(1500, 'A') + "\n%%EOF\n");
  config_->SetIndexedDirectories({dir_a_.string()});
  config_->SetSupportedExtensions({"txt", "pdf"});
  config_->set_enable_embedded_image_ocr(false);

  ASSERT_GE(pipeline_->PerformInitialScan(), 1u);
  EXPECT_EQ(pipeline_->GetPhase(), OrchestratorPhase::kIdle);
  ASSERT_TRUE(pipeline_->RebuildIndex());
  EXPECT_EQ(pipeline_->GetPhase(), OrchestratorPhase::kIdle);

  const PipelineStats baseline_stats = pipeline_->GetStats();
  ASSERT_GE(baseline_stats.total_parse_failures, 1u);

  // 2. Normal incremental rescan skips unchanged .txt and already-failed .pdf.
  pipeline_->StartBackgroundTask(/*rebuild=*/false, /*force_incremental=*/true, {});
  ASSERT_TRUE(WaitUntil([&]() { return !pipeline_->IsIndexing(); }));
  const PipelineStats after_normal_inc = pipeline_->GetStats();
  EXPECT_EQ(after_normal_inc.total_files_indexed, baseline_stats.total_files_indexed);
  EXPECT_EQ(after_normal_inc.total_parse_failures, baseline_stats.total_parse_failures);

  // 3. Enabling OCR sets ocr_expanded = true: ocr_candidate.pdf is re-dispatched, .txt stays skipped.
  const IndexConfigSnapshot before_ocr = config_->CaptureSnapshot();
  config_->set_enable_embedded_image_ocr(true);
  const ConfigChangeSummary ocr_diff = DiffIndexConfig(before_ocr, config_->CaptureSnapshot());
  ASSERT_TRUE(ocr_diff.ocr_expanded);
  pipeline_->ApplyConfigChanges(ocr_diff, {});
  ASSERT_TRUE(WaitUntil([&]() { return !pipeline_->IsIndexing(); }));
  EXPECT_EQ(pipeline_->GetPhase(), OrchestratorPhase::kIdle);

  const PipelineStats after_ocr = pipeline_->GetStats();
  EXPECT_EQ(after_ocr.total_files_indexed, baseline_stats.total_files_indexed);
  EXPECT_EQ(after_ocr.total_parse_failures, baseline_stats.total_parse_failures + 1);
}

TEST_F(IndexStateMachineTest, PersistedIndexStateCrashRecoveryAndColdBoot) {
  InitPipeline();
  WriteFile(dir_a_ / "recover.txt", "crash_recovery_token");
  config_->SetIndexedDirectories({dir_a_.string()});
  config_->SetSupportedExtensions({"txt"});

  // 1. Simulate process crash mid-scan ('scanning' persisted in index_meta).
  ASSERT_TRUE(storage_->SetMetaValue("index_state", "scanning"));
  auto rebooted_scan_pipe = std::make_shared<IndexPipeline>(config_, worker_guard_, storage_);
  ASSERT_TRUE(rebooted_scan_pipe->Init());
  EXPECT_EQ(rebooted_scan_pipe->GetPersistedIndexState(), PersistedIndexState::kScanning);
  EXPECT_FALSE(rebooted_scan_pipe->HasIncompleteRebuild());

  // Clean scan heals 'scanning' -> 'ready'.
  ASSERT_EQ(rebooted_scan_pipe->PerformInitialScan(), 1u);
  EXPECT_EQ(rebooted_scan_pipe->GetPersistedIndexState(), PersistedIndexState::kReady);
  std::string state_val;
  ASSERT_TRUE(storage_->GetMetaValue("index_state", &state_val));
  EXPECT_EQ(state_val, "ready");
  rebooted_scan_pipe.reset();

  // 2. Simulate process crash mid-rebuild ('rebuilding_incomplete' persisted in index_meta).
  ASSERT_TRUE(storage_->SetMetaValue("index_state", "rebuilding_incomplete"));
  ASSERT_TRUE(storage_->SetMetaValue("rebuild_incomplete", "1"));
  auto rebooted_rebuild_pipe = std::make_shared<IndexPipeline>(config_, worker_guard_, storage_);
  ASSERT_TRUE(rebooted_rebuild_pipe->Init());
  EXPECT_EQ(rebooted_rebuild_pipe->GetPersistedIndexState(), PersistedIndexState::kRebuildingIncomplete);
  EXPECT_TRUE(rebooted_rebuild_pipe->HasIncompleteRebuild());

  // Clean background incremental scan heals 'rebuilding_incomplete' -> 'ready'.
  rebooted_rebuild_pipe->StartBackgroundTask(/*rebuild=*/false, /*force_incremental=*/true, {});
  ASSERT_TRUE(WaitUntil([&]() { return !rebooted_rebuild_pipe->IsIndexing(); }));
  EXPECT_EQ(rebooted_rebuild_pipe->GetPersistedIndexState(), PersistedIndexState::kReady);
  EXPECT_FALSE(rebooted_rebuild_pipe->HasIncompleteRebuild());
  ASSERT_TRUE(storage_->GetMetaValue("index_state", &state_val));
  EXPECT_EQ(state_val, "ready");
}

}  // namespace
}  // namespace service
}  // namespace soeasy
