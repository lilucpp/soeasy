#ifndef SOEASY_SRC_SERVICE_DOC_PRECHECKER_H_
#define SOEASY_SRC_SERVICE_DOC_PRECHECKER_H_

#include <cstdint>
#include <string>

namespace soeasy {
namespace service {

enum class PrecheckStatus {
  kValid = 0,               // Format is valid and ready for parsing
  kEmpty = 1,               // 0 bytes, skip body parsing
  kTruncatedOrCorrupt = 2,  // Missing EOCD or corrupted compound structures
  kEncrypted = 3,           // Password-protected document
  kDisguised = 4            // Extension does not match file signature
};

struct PrecheckResult {
  PrecheckStatus status = PrecheckStatus::kValid;
  bool has_images = false;
  uint32_t recommended_timeout_ms = 15000;
  std::string reason;
};

class DocPrechecker {
 public:
  /**
   * @brief Fast, non-blocking sanity check on a document before dispatching to worker.
   *
   * Verifies file magic, checks for truncated ZIP archives (missing EOCD),
   * detects password-encrypted documents (OLE2 EncryptedPackage, PDF /Encrypt),
   * and dynamically computes recommended timeout based on embedded images/scanned status.
   *
   * @param file_path Absolute path to document
   * @param ext Lowercase extension without dot
   * @param file_size Size on disk in bytes
   * @return PrecheckResult containing status and timeout recommendation
   */
  static PrecheckResult Check(const std::string& file_path, const std::string& ext,
                              uint64_t file_size);
};

}  // namespace service
}  // namespace soeasy

#endif  // SOEASY_SRC_SERVICE_DOC_PRECHECKER_H_
