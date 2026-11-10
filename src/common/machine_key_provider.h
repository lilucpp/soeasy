// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_COMMON_MACHINE_KEY_PROVIDER_H_
#define SOEASY_SRC_COMMON_MACHINE_KEY_PROVIDER_H_

#include <string>

namespace soeasy {
namespace common {

struct KeyRetrievalResult {
  bool success = false;
  std::string encryption_key;
  std::string error_message;
  bool is_newly_generated = false;
};

/**
 * @brief Zero-friction machine-bound encryption key provider.
 *
 * Implements envelope encryption (DEK + KEK) bound to the operating system's
 * lifecycle-immutable machine identifier (/etc/machine-id on Linux, DPAPI/MachineGuid on Windows).
 * Guarantees 100% key stability across reboots, hardware changes, and kernel upgrades.
 */
class MachineKeyProvider {
 public:
  static constexpr const char *kDefaultLegacyKey = "soeasy_master_page_key_2026";
  static constexpr const char *kKeyFileName = ".db_envelope.key";

  /**
   * @brief Get or create machine-bound persistent database encryption key.
   *
   * @param app_data_dir Application data directory (e.g. ~/.local/share/soeasy or ~/.soeasy)
   * @return KeyRetrievalResult with status and the decrypted high-entropy DEK
   */
  static KeyRetrievalResult GetOrCreateDatabaseKey(const std::string &app_data_dir);

  /**
   * @brief Retrieve the host machine's lifecycle-stable identifier (/etc/machine-id on Linux).
   */
  static std::string GetMachineLifecycleId();

  /**
   * @brief Seal DEK into an envelope string bound to the specified machine_id.
   */
  static std::string SealKey(const std::string &raw_dek, const std::string &machine_id);

  /**
   * @brief Unseal DEK from an envelope string using the specified machine_id.
   * Returns true on success (matching machine_id and valid HMAC/checksum).
   */
  static bool UnsealKey(const std::string &envelope, const std::string &machine_id, std::string *out_dek);

  /**
   * @brief Securely zero sensitive memory.
   */
  static void CleanseMemory(std::string &secret);

 private:
  static bool GenerateCryptographicRandom(unsigned char *buf, size_t len);
  static std::string GenerateRandomHex(size_t bytes);
  static bool SetRestrictiveFilePermissions(const std::string &file_path);
};

}  // namespace common
}  // namespace soeasy

#endif  // SOEASY_SRC_COMMON_MACHINE_KEY_PROVIDER_H_
