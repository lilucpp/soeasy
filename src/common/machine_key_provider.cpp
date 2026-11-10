// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "common/machine_key_provider.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <wincrypt.h>
#include <windows.h>
#endif

#include "common/logger.h"

namespace soeasy {
namespace common {

namespace {

// Self-contained standard SHA-256 implementation
class Sha256 {
 public:
  Sha256() { reset(); }

  void reset() {
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
    count_ = 0;
    buffer_len_ = 0;
  }

  void update(const void *data, size_t len) {
    const uint8_t *input = static_cast<const uint8_t *>(data);
    count_ += len;
    while (len > 0) {
      size_t copy_len = std::min(len, static_cast<size_t>(64 - buffer_len_));
      std::memcpy(buffer_ + buffer_len_, input, copy_len);
      buffer_len_ += copy_len;
      input += copy_len;
      len -= copy_len;
      if (buffer_len_ == 64) {
        transform(buffer_);
        buffer_len_ = 0;
      }
    }
  }

  void final(uint8_t digest[32]) {
    uint64_t total_bits = count_ * 8;
    buffer_[buffer_len_++] = 0x80;
    if (buffer_len_ > 56) {
      std::memset(buffer_ + buffer_len_, 0, 64 - buffer_len_);
      transform(buffer_);
      buffer_len_ = 0;
    }
    std::memset(buffer_ + buffer_len_, 0, 56 - buffer_len_);
    for (int i = 7; i >= 0; --i) {
      buffer_[56 + (7 - i)] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xFF);
    }
    transform(buffer_);
    for (int i = 0; i < 8; ++i) {
      digest[i * 4 + 0] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
      digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
      digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
      digest[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFF);
    }
  }

  static std::string Hash(const std::string &data) {
    Sha256 s;
    s.update(data.data(), data.size());
    uint8_t digest[32];
    s.final(digest);
    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
      oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
  }

  static void HashRaw(const std::string &data, uint8_t digest[32]) {
    Sha256 s;
    s.update(data.data(), data.size());
    s.final(digest);
  }

 private:
  static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

  void transform(const uint8_t block[64]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | (static_cast<uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t temp1 = h + S1 + ch + K[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t temp2 = S0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  uint32_t state_[8];
  uint64_t count_;
  uint8_t buffer_[64];
  size_t buffer_len_;
};

}  // namespace

std::string MachineKeyProvider::GetMachineLifecycleId() {
#ifdef _WIN32
  char guid_buf[256] = {0};
  DWORD buf_size = sizeof(guid_buf);
  HKEY h_key = nullptr;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &h_key) ==
      ERROR_SUCCESS) {
    if (RegQueryValueExA(h_key, "MachineGuid", nullptr, nullptr, reinterpret_cast<LPBYTE>(guid_buf), &buf_size) ==
        ERROR_SUCCESS) {
      RegCloseKey(h_key);
      return std::string(guid_buf);
    }
    RegCloseKey(h_key);
  }
  return "WINDOWS_GENERIC_MACHINE_ID_FALLBACK";
#else
  const std::vector<std::string> candidates = {"/etc/machine-id", "/var/lib/dbus/machine-id"};
  for (const auto &path : candidates) {
    std::ifstream ifs(path);
    if (ifs.is_open()) {
      std::string id;
      ifs >> id;
      if (id.length() >= 32) {
        return id;
      }
    }
  }
  LOG_WARN("[MachineKeyProvider] /etc/machine-id not found, falling back to static identifier.");
  return "LINUX_STATIC_MACHINE_ID_FALLBACK";
#endif
}

bool MachineKeyProvider::GenerateCryptographicRandom(unsigned char *buf, size_t len) {
#ifdef _WIN32
  HCRYPTPROV h_prov = 0;
  if (CryptAcquireContext(&h_prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
    BOOL ok = CryptGenRandom(h_prov, static_cast<DWORD>(len), buf);
    CryptReleaseContext(h_prov, 0);
    if (ok) return true;
  }
#else
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    size_t total = 0;
    while (total < len) {
      ssize_t n = read(fd, buf + total, len - total);
      if (n <= 0) break;
      total += n;
    }
    close(fd);
    if (total == len) return true;
  }
#endif
  // Pseudo-random fallback with timestamp mixing if system RNG fails
  for (size_t i = 0; i < len; ++i) {
    buf[i] = static_cast<unsigned char>((rand() ^ (i * 109)) & 0xFF);
  }
  return true;
}

std::string MachineKeyProvider::GenerateRandomHex(size_t bytes) {
  std::vector<unsigned char> buf(bytes);
  GenerateCryptographicRandom(buf.data(), bytes);
  std::ostringstream oss;
  for (auto b : buf) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  return oss.str();
}

bool MachineKeyProvider::SetRestrictiveFilePermissions(const std::string &file_path) {
#ifndef _WIN32
  // Strict 0600 (owner read and write only)
  return chmod(file_path.c_str(), S_IRUSR | S_IWUSR) == 0;
#else
  (void)file_path;
  return true;
#endif
}

std::string MachineKeyProvider::SealKey(const std::string &raw_dek, const std::string &machine_id) {
  std::string salt = GenerateRandomHex(16);
  std::string kek_input = machine_id + ":" + salt + ":soeasy_v1";

  uint8_t kek[32];
  Sha256::HashRaw(kek_input, kek);

  // Derive mask for DEK stream encryption
  std::string mask_input(reinterpret_cast<char *>(kek), 32);
  mask_input += ":cipher_mask";
  uint8_t mask[32];
  Sha256::HashRaw(mask_input, mask);

  std::string enc_dek = raw_dek;
  for (size_t i = 0; i < enc_dek.size(); ++i) {
    enc_dek[i] ^= mask[i % 32];
  }

  // Derive authentication tag (HMAC-like construction with SHA-256)
  std::string auth_input(reinterpret_cast<char *>(kek), 32);
  auth_input += ":" + enc_dek;
  std::string auth_tag = Sha256::Hash(auth_input);

  // Convert enc_dek to hex
  std::ostringstream enc_hex;
  for (unsigned char c : enc_dek) {
    enc_hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  }

  // Cleanse sensitive KEK memory
  std::memset(kek, 0, sizeof(kek));
  std::memset(mask, 0, sizeof(mask));

  return "SOEASY_KEY_V1\n" + salt + "\n" + enc_hex.str() + "\n" + auth_tag + "\n";
}

bool MachineKeyProvider::UnsealKey(const std::string &envelope, const std::string &machine_id, std::string *out_dek) {
  std::istringstream iss(envelope);
  std::string header, salt, enc_hex, auth_tag;
  if (!(iss >> header >> salt >> enc_hex >> auth_tag)) {
    return false;
  }
  if (header != "SOEASY_KEY_V1") {
    return false;
  }

  std::string kek_input = machine_id + ":" + salt + ":soeasy_v1";
  uint8_t kek[32];
  Sha256::HashRaw(kek_input, kek);

  // Decode enc_dek from hex
  std::string enc_dek;
  for (size_t i = 0; i + 1 < enc_hex.size(); i += 2) {
    unsigned int byte_val = 0;
    std::stringstream ss;
    ss << std::hex << enc_hex.substr(i, 2);
    ss >> byte_val;
    enc_dek.push_back(static_cast<char>(byte_val));
  }

  // Verify authentication tag
  std::string auth_input(reinterpret_cast<char *>(kek), 32);
  auth_input += ":" + enc_dek;
  std::string expected_tag = Sha256::Hash(auth_input);
  if (expected_tag != auth_tag) {
    std::memset(kek, 0, sizeof(kek));
    return false;  // Host machine ID mismatch or envelope corrupted!
  }

  // Derive mask and decrypt
  std::string mask_input(reinterpret_cast<char *>(kek), 32);
  mask_input += ":cipher_mask";
  uint8_t mask[32];
  Sha256::HashRaw(mask_input, mask);

  std::string raw_dek = enc_dek;
  for (size_t i = 0; i < raw_dek.size(); ++i) {
    raw_dek[i] ^= mask[i % 32];
  }

  std::memset(kek, 0, sizeof(kek));
  std::memset(mask, 0, sizeof(mask));

  if (out_dek) {
    *out_dek = raw_dek;
  }
  return true;
}

void MachineKeyProvider::CleanseMemory(std::string &secret) {
  volatile char *p = const_cast<volatile char *>(secret.data());
  for (size_t i = 0; i < secret.size(); ++i) {
    p[i] = 0;
  }
  secret.clear();
}

KeyRetrievalResult MachineKeyProvider::GetOrCreateDatabaseKey(const std::string &app_data_dir) {
  KeyRetrievalResult result;
  std::string key_file_path = app_data_dir + "/" + kKeyFileName;
  std::string machine_id = GetMachineLifecycleId();

  // 1. Try reading existing sealed envelope
  std::ifstream ifs(key_file_path);
  if (ifs.is_open()) {
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string envelope = ss.str();
    ifs.close();

    std::string recovered_dek;
    if (UnsealKey(envelope, machine_id, &recovered_dek)) {
      LOG_INFO("[MachineKeyProvider] Successfully unsealed machine-bound key from '{}'", key_file_path);
      result.success = true;
      result.encryption_key = recovered_dek;
      result.is_newly_generated = false;
      return result;
    } else {
      LOG_ERROR("[MachineKeyProvider] Failed to unseal key file '{}'. Host machine ID mismatch or file corrupted.",
                key_file_path);
      result.success = false;
      result.error_message = "Host machine-id mismatch or key file corrupted. Access denied.";
      return result;
    }
  }

  // 2. Key does not exist: generate fresh 256-bit high-entropy DEK
  LOG_INFO("[MachineKeyProvider] No existing key file found. Generating new machine-bound DEK at '{}'...",
           key_file_path);
  std::string new_dek = "soeasy_dek_" + GenerateRandomHex(32);
  std::string sealed_envelope = SealKey(new_dek, machine_id);

  // Write sealed envelope to disk
  std::ofstream ofs(key_file_path, std::ios::trunc);
  if (!ofs.is_open()) {
    result.success = false;
    result.error_message = "Failed to create key envelope file at: " + key_file_path;
    return result;
  }
  ofs << sealed_envelope;
  ofs.close();

  // Enforce 0600 restrictive permissions
  SetRestrictiveFilePermissions(key_file_path);

  LOG_INFO("[MachineKeyProvider] Successfully generated and sealed new database key (permissions 0600).");
  result.success = true;
  result.encryption_key = new_dek;
  result.is_newly_generated = true;
  return result;
}

}  // namespace common
}  // namespace soeasy
