// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_PARSER_REGISTRY_H_
#define SOEASY_SRC_WORKER_PARSER_REGISTRY_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "worker/idocument_parser.h"

namespace soeasy {
namespace worker {

/**
 * @brief Registry and responsibility chain coordinator for document parsers.
 *
 * Maintains an ordered sequence of IDocumentParser instances sorted by Priority().
 * Dispatches extraction requests to the first matching parser in the chain.
 */
class ParserRegistry {
 public:
  ParserRegistry() = default;
  ~ParserRegistry() = default;

  ParserRegistry(const ParserRegistry &) = delete;
  ParserRegistry &operator=(const ParserRegistry &) = delete;
  ParserRegistry(ParserRegistry &&) noexcept = default;
  ParserRegistry &operator=(ParserRegistry &&) noexcept = default;

  /**
   * @brief Register a parser implementation into the registry.
   * Maintains internal sorting by Priority() in descending order.
   */
  void RegisterParser(std::unique_ptr<IDocumentParser> parser);

  /**
   * @brief Execute parser responsibility chain for the given file.
   *
   * Finds the highest-priority parser whose CanHandle(ctx) returns true,
   * then delegates the extraction to its Parse method.
   *
   * @param ctx Document parameters
   * @param out_doc Destination document receiving extracted text
   * @return true on parse success, false if parse failed or no matching parser found
   */
  bool ParseDocument(const ParseContext &ctx, ExtractedDocument *out_doc);

  /**
   * @brief Check whether any registered parser can handle the given file extension.
   *
   * @param ext Lowercase file extension without dot
   */
  bool IsSupported(const std::string &ext) const;

  /**
   * @brief Periodically trigger idle unload checks across all registered parsers.
   *
   * @param idle_seconds Duration of idleness required before unloading models
   */
  void CheckIdleUnload(double idle_seconds = 60.0);

  /**
   * @brief Return number of registered parsers.
   */
  size_t size() const;

  /**
   * @brief Factory method creating and populating the default production parser registry:
   * L3 Image Parser (Priority 30) -> L2 Office Parser (Priority 20) -> L1 Text Parser (Priority 10).
   *
   * @param models_dir Directory containing ONNX OCR models (empty to auto-discover)
   */
  static std::unique_ptr<ParserRegistry> CreateDefaultRegistry(const std::string &models_dir = "");

 private:
  std::vector<std::unique_ptr<IDocumentParser>> parsers_;
  mutable std::mutex mutex_;
};

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_PARSER_REGISTRY_H_
