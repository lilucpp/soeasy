// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "service/index_pipeline.h"

#include "service/index_orchestrator.h"
#include "service/search_coordinator.h"
#include "service/worker_guard.h"
#include "storage/istorage_engine.h"

namespace soeasy {
namespace service {

int64_t IndexPipeline::GenerateDocId(const std::string &path) {
  return IndexOrchestrator::GenerateDocId(path);
}

IndexPipeline::IndexPipeline(std::shared_ptr<IndexConfig> config, std::shared_ptr<WorkerGuard> worker_guard,
                             std::shared_ptr<storage::IStorageEngine> storage_engine)
    : config_(std::move(config)), worker_guard_(std::move(worker_guard)), storage_engine_(std::move(storage_engine)) {}

IndexPipeline::~IndexPipeline() = default;

bool IndexPipeline::Init() {
  if (!config_ || !worker_guard_ || !storage_engine_) {
    return false;
  }

  name_index_ = std::make_shared<NameIndex>();
  orchestrator_ = std::make_shared<IndexOrchestrator>(config_, worker_guard_, storage_engine_, name_index_);
  if (!orchestrator_->Init()) {
    return false;
  }

  search_coordinator_ = std::make_shared<SearchCoordinator>(name_index_, storage_engine_);
  return true;
}

bool IndexPipeline::StartLiveWatch() {
  return orchestrator_ ? orchestrator_->StartLiveWatch() : false;
}

void IndexPipeline::StopLiveWatch() {
  if (orchestrator_) {
    orchestrator_->StopLiveWatch();
  }
}

bool IndexPipeline::AddAndScanDirectory(const std::string &dir_path, bool recursive) {
  return orchestrator_ ? orchestrator_->AddAndScanDirectory(dir_path, recursive) : false;
}

bool IndexPipeline::RemoveDirectoryIndex(const std::string &dir_path) {
  return orchestrator_ ? orchestrator_->RemoveDirectoryIndex(dir_path) : false;
}

void IndexPipeline::TriggerAsyncSpaceReclaim() {
  if (orchestrator_) {
    orchestrator_->TriggerAsyncSpaceReclaim();
  }
}

size_t IndexPipeline::PerformInitialScan(ScanProgressCallback progress_cb) {
  return orchestrator_ ? orchestrator_->PerformInitialScan(std::move(progress_cb)) : 0;
}

bool IndexPipeline::RebuildIndex(ScanProgressCallback progress_cb) {
  return orchestrator_ ? orchestrator_->RebuildIndex(std::move(progress_cb)) : false;
}

bool IndexPipeline::HandleFileChange(const DebouncedEvent &event) {
  return orchestrator_ ? orchestrator_->HandleFileChange(event) : false;
}

std::vector<NameMatchResult> IndexPipeline::SearchFilename(const std::string &query, int limit, int offset,
                                                           const std::string &path_prefix, int *out_total_count) const {
  if (!search_coordinator_) {
    if (out_total_count) *out_total_count = 0;
    return {};
  }
  return search_coordinator_->SearchFilename(query, limit, offset, path_prefix, out_total_count);
}

std::vector<SearchResultItem> IndexPipeline::SearchFulltext(const std::string &query, int limit, int offset,
                                                            const std::string &path_prefix, bool sync_flush,
                                                            int *out_total_count) {
  if (sync_flush && orchestrator_) {
    orchestrator_->FlushWriteQueue();
  }
  if (!search_coordinator_) {
    if (out_total_count) *out_total_count = 0;
    return {};
  }
  return search_coordinator_->SearchFulltext(query, limit, offset, path_prefix, out_total_count);
}

bool IndexPipeline::GetDocumentContent(int64_t doc_id, std::string *out_content) const {
  if (!search_coordinator_) {
    return false;
  }
  return search_coordinator_->GetDocumentContent(doc_id, out_content);
}

void IndexPipeline::FlushWriteQueue() {
  if (orchestrator_) {
    orchestrator_->FlushWriteQueue();
  }
}

void IndexPipeline::ApplyRuntimeConfig() {
  if (orchestrator_) {
    orchestrator_->ApplyRuntimeConfig();
  }
}

void IndexPipeline::ApplyConfigChanges(const ConfigChangeSummary &diff, BackgroundIndexCallbacks callbacks) {
  if (orchestrator_) {
    orchestrator_->ApplyConfigChanges(diff, std::move(callbacks));
  }
}

size_t IndexPipeline::PruneStaleEntriesByConfig() {
  return orchestrator_ ? orchestrator_->PruneStaleEntriesByConfig() : 0;
}

void IndexPipeline::SyncWatchedDirectories() {
  if (orchestrator_) {
    orchestrator_->SyncWatchedDirectories();
  }
}

void IndexPipeline::StartBackgroundTask(bool rebuild, bool force_incremental, BackgroundIndexCallbacks callbacks) {
  if (orchestrator_) {
    orchestrator_->StartBackgroundTask(rebuild, force_incremental, std::move(callbacks));
  }
}

void IndexPipeline::ClearBackgroundCallbacks() {
  if (orchestrator_) {
    orchestrator_->ClearBackgroundCallbacks();
  }
}

bool IndexPipeline::IsIndexing() const {
  return orchestrator_ ? orchestrator_->IsIndexing() : false;
}

OrchestratorPhase IndexPipeline::GetPhase() const {
  return orchestrator_ ? orchestrator_->GetPhase() : OrchestratorPhase::kIdle;
}

bool IndexPipeline::HasIncompleteRebuild() const {
  return orchestrator_ ? orchestrator_->HasIncompleteRebuild() : false;
}

size_t IndexPipeline::IndexedFileCount() const {
  return orchestrator_ ? orchestrator_->IndexedFileCount() : 0;
}

void IndexPipeline::CancelScan() {
  if (orchestrator_) {
    orchestrator_->CancelScan();
  }
}

bool IndexPipeline::is_scan_cancelled() const {
  return orchestrator_ ? orchestrator_->is_scan_cancelled() : false;
}

PipelineStats IndexPipeline::GetStats() const {
  return orchestrator_ ? orchestrator_->GetStats() : PipelineStats{};
}

}  // namespace service
}  // namespace soeasy
