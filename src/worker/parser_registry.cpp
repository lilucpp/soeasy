// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/parser_registry.h"

#include <algorithm>
#include <utility>

#include "common/logger.h"
#include "worker/l1_text_parser.h"
#include "worker/l2_office_parser.h"
#include "worker/l3_image_parser.h"

namespace soeasy {
namespace worker {

void ParserRegistry::RegisterParser(std::unique_ptr<IDocumentParser> parser) {
  if (!parser) return;
  std::lock_guard<std::mutex> lock(mutex_);
  parsers_.push_back(std::move(parser));
  // Sort descending by Priority (highest priority evaluated first)
  std::stable_sort(parsers_.begin(), parsers_.end(),
                   [](const std::unique_ptr<IDocumentParser> &a, const std::unique_ptr<IDocumentParser> &b) {
                     return a->Priority() > b->Priority();
                   });
}

bool ParserRegistry::ParseDocument(const ParseContext &ctx, ExtractedDocument *out_doc) {
  if (!out_doc) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &parser : parsers_) {
    if (parser->CanHandle(ctx)) {
      return parser->Parse(ctx, out_doc);
    }
  }
  LOG_WARN("[ParserRegistry] No registered parser claimed file '{}' (ext: '{}')", ctx.file_path, ctx.ext);
  return false;
}

bool ParserRegistry::IsSupported(const std::string &ext) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ParseContext dummy;
  dummy.ext = ext;
  for (const auto &parser : parsers_) {
    if (parser->CanHandle(dummy)) {
      return true;
    }
  }
  return false;
}

void ParserRegistry::CheckIdleUnload(double idle_seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &parser : parsers_) {
    parser->CheckIdleUnload(idle_seconds);
  }
}

size_t ParserRegistry::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return parsers_.size();
}

std::unique_ptr<ParserRegistry> ParserRegistry::CreateDefaultRegistry(const std::string &models_dir) {
  auto registry = std::make_unique<ParserRegistry>();
  registry->RegisterParser(std::make_unique<L3ImageParser>(ocr::OcrMode::kNormal, models_dir));
  registry->RegisterParser(std::make_unique<L2OfficeParser>());
  registry->RegisterParser(std::make_unique<L1TextParser>());
  return registry;
}

}  // namespace worker
}  // namespace soeasy
