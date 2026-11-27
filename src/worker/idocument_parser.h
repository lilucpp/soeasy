// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_IDOCUMENT_PARSER_H_
#define SOEASY_SRC_WORKER_IDOCUMENT_PARSER_H_

#include <cstdint>
#include <istream>
#include <string>

namespace soeasy {
namespace worker {

struct ParseContext {
  int64_t doc_id = 0;
  std::string file_path;
  std::string ext;  // Lowercase without dot, e.g. "docx", "cpp", "png"
  uint64_t file_size = 0;
  uint32_t timeout_ms = 15000;
  bool enable_embedded_ocr = false;  // Trigger embedded image OCR in Office / PDF
};

enum class ParseOutcome {
  kOk = 0,
  kEncrypted = 1,
  kFailed = 2,
  kPartialOcr = 3,
  kEmptyContent = 4,
};

struct ExtractedDocument {
  std::string utf8_text;        // Normalized UTF-8 text string
  bool is_scanned_pdf = false;  // True if scanned / image-only PDF
  size_t page_count = 0;        // Document page count (if applicable)
  std::string metadata_json;    // Optional extra metadata (JSON formatted)
  ParseOutcome outcome = ParseOutcome::kOk;
  std::string error_message;    // Specific failure or encryption reason
};

/**
 * @brief Unified polymorphic interface for document text and metadata extraction.
 *
 * All specialized format extractors (L1 Plain Text, L2 Office/PDF, L3 Image OCR,
 * and future L4 Archive extractors) implement this interface to be registered in
 * the ParserRegistry responsibility chain.
 */
class IDocumentParser {
 public:
  virtual ~IDocumentParser() = default;

  /**
   * @brief Check whether this parser can handle the given document context.
   *
   * @param ctx Parsing parameters including path, extension, and file size
   * @return true if this parser claims capability to parse the file
   */
  [[nodiscard]] virtual bool CanHandle(const ParseContext &ctx) const = 0;

  /**
   * @brief Parse document and extract UTF-8 text and metadata.
   *
   * @param ctx Parsing execution parameters
   * @param out_doc Destination document receiving extracted plain text and flags
   * @return true on success (or clean skip on filtered images), false on fatal parse failure
   */
  [[nodiscard]] virtual bool Parse(const ParseContext &ctx, ExtractedDocument *out_doc) = 0;

  /**
   * @brief Parse document directly from an in-memory byte buffer without disk I/O.
   *
   * @param raw_bytes Pointer to raw file bytes
   * @param raw_size Byte length of raw_bytes
   * @param ctx Parsing execution parameters (virtual path, lowercase ext, timeout_ms)
   * @param out_doc Destination document receiving extracted plain text and flags
   * @return true on success, false on fatal parse failure
   */
  [[nodiscard]] virtual bool ParseBuffer(const char * /*raw_bytes*/, size_t /*raw_size*/, const ParseContext & /*ctx*/,
                                         ExtractedDocument * /*out_doc*/) {
    return false;
  }

  /**
   * @brief Parse document directly from a stream.
   * Enables stream processing, standard input pipe (cat file | parser), and memory-backed streams.
   *
   * @param stream Input byte stream
   * @param ctx Parsing execution parameters
   * @param out_doc Destination document receiving extracted plain text and flags
   * @return true on success, false on fatal parse failure
   */
  [[nodiscard]] virtual bool ParseStream(std::istream & /*stream*/, const ParseContext & /*ctx*/,
                                         ExtractedDocument * /*out_doc*/) {
    return false;
  }

  /**
   * @brief Parser priority in the responsibility chain.
   * Higher priority parsers are evaluated first.
   * Standard priorities: L3 Image OCR (30) > L2 Office/PDF (20) > L1 Text (10).
   */
  [[nodiscard]] virtual int Priority() const { return 0; }

  /**
   * @brief Unique identifier name of the parser implementation.
   */
  [[nodiscard]] virtual std::string Name() const = 0;

  /**
   * @brief Periodic hook to perform idle unload or cache trimming of heavy resources.
   */
  virtual void CheckIdleUnload(double /*idle_seconds*/) {}
};

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_IDOCUMENT_PARSER_H_
