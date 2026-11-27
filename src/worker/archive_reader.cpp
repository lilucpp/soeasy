// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/archive_reader.h"

#include <archive.h>
#include <archive_entry.h>
#ifndef _WIN32
#include <locale.h>
#endif
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>

#include "common/encoding_utils.h"
#include "common/format_categories.h"
#include "common/logger.h"
#include "doctotext_c_api.h"
#include "utf8.h"

namespace soeasy {
namespace worker {

namespace {

#ifndef S_IFMT
#define S_IFMT 0170000
#define S_IFREG 0100000
#endif

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string ExtractExtLower(const std::string &name) {
  const size_t slash = name.find_last_of("/\\");
  const std::string base = (slash == std::string::npos) ? name : name.substr(slash + 1);
  const size_t dot = base.find_last_of('.');
  if (dot == std::string::npos || dot + 1 >= base.size()) {
    return "";
  }
  return ToLowerAscii(base.substr(dot + 1));
}

bool IsContainerOrArchiveExt(const std::string &ext) {
  return common::IsSupportedArchiveExtension(ext) || ext == "iso";
}

bool IsSingleStreamCompressionExt(const std::string &ext) {
  return ext == "gz" || ext == "bz2" || ext == "xz" || ext == "lzma";
}

bool HasMagic(const unsigned char *buf, size_t len, const unsigned char *sig, size_t sig_len) {
  return len >= sig_len && std::memcmp(buf, sig, sig_len) == 0;
}

bool IsArchiveMagicBytes(const unsigned char *buf, size_t len, std::string *out_ext = nullptr) {
  static constexpr unsigned char kRar5Sig[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00};
  static constexpr unsigned char kRar4Sig[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
  static constexpr unsigned char k7zSig[] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
  static constexpr unsigned char kZipSig[] = {0x50, 0x4B, 0x03, 0x04};
  static constexpr unsigned char kZipEmptySig[] = {0x50, 0x4B, 0x05, 0x06};
  static constexpr unsigned char kGzSig[] = {0x1F, 0x8B, 0x08};
  static constexpr unsigned char kBz2Sig[] = {0x42, 0x5A, 0x68};
  static constexpr unsigned char kXzSig[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};

  if (HasMagic(buf, len, kRar5Sig, sizeof(kRar5Sig)) || HasMagic(buf, len, kRar4Sig, sizeof(kRar4Sig))) {
    if (out_ext) *out_ext = "rar";
    return true;
  }
  if (HasMagic(buf, len, k7zSig, sizeof(k7zSig))) {
    if (out_ext) *out_ext = "7z";
    return true;
  }
  if (HasMagic(buf, len, kGzSig, sizeof(kGzSig))) {
    if (out_ext) *out_ext = "gz";
    return true;
  }
  if (HasMagic(buf, len, kBz2Sig, sizeof(kBz2Sig))) {
    if (out_ext) *out_ext = "bz2";
    return true;
  }
  if (HasMagic(buf, len, kXzSig, sizeof(kXzSig))) {
    if (out_ext) *out_ext = "xz";
    return true;
  }
  // POSIX tar ustar magic at offset 257
  if (len >= 263 && std::memcmp(buf + 257, "ustar", 5) == 0) {
    if (out_ext) *out_ext = "tar";
    return true;
  }
  // Note: OOXML/ODF also start with PK\x03\x04; only treat as nested zip if not obviously OOXML/ODF
  if (HasMagic(buf, len, kZipSig, sizeof(kZipSig)) || HasMagic(buf, len, kZipEmptySig, sizeof(kZipEmptySig))) {
    doctotext_parser_type pt = DOCTOTEXT_PARSER_AUTO;
    if (doctotext_parser_type_by_file_content_from_buffer(reinterpret_cast<const char *>(buf), len, &pt) == 1 &&
        (pt == DOCTOTEXT_PARSER_ODF_OOXML || pt == DOCTOTEXT_PARSER_IWORK)) {
      return false;
    }
    if (out_ext) *out_ext = "zip";
    return true;
  }
  return false;
}

bool LooksLikeEncryptionError(const char *err_cstr) {
  if (!err_cstr) return false;
  std::string lower = ToLowerAscii(err_cstr);
  return lower.find("encrypt") != std::string::npos || lower.find("passphrase") != std::string::npos ||
         lower.find("password") != std::string::npos;
}

/// Sanitize and normalize an entry relative path inside an archive.
std::string NormalizeArchiveRelPath(const std::string &raw_rel) {
  std::string s = raw_rel;
  std::replace(s.begin(), s.end(), '\\', '/');
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == '/') ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && s[j] != '/') ++j;
    std::string seg = s.substr(i, j - i);
    i = j;
    if (seg == "." || seg.empty()) continue;
    if (seg == "..") {
      if (!parts.empty()) parts.pop_back();
      continue;
    }
    parts.push_back(std::move(seg));
  }
  std::string out;
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k > 0) out.push_back('/');
    out += parts[k];
  }
  return out;
}

std::string ExtractEntryRelPathUtf8(archive_entry *entry) {
  const char *u8 = archive_entry_pathname_utf8(entry);
  if (u8 && *u8 && utf8::is_valid(u8, u8 + std::strlen(u8))) {
    return NormalizeArchiveRelPath(u8);
  }
  const wchar_t *wpath = archive_entry_pathname_w(entry);
  if (wpath && *wpath) {
    std::string conv = common::EncodingUtils::WideToUtf8(wpath);
    if (!conv.empty() && utf8::is_valid(conv.begin(), conv.end())) {
      return NormalizeArchiveRelPath(conv);
    }
  }
  const char *raw = archive_entry_pathname(entry);
  if (raw && *raw) {
    return NormalizeArchiveRelPath(common::EncodingUtils::ToUtf8(raw));
  }
  return "";
}

std::string ExtractBasename(const std::string &path) {
  const size_t pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

std::string SniffExtensionFromBuffer(const char *buf, size_t len) {
  if (!buf || len == 0) return "";
  doctotext_parser_type pt = DOCTOTEXT_PARSER_AUTO;
  if (doctotext_parser_type_by_file_content_from_buffer(buf, len, &pt) == 1) {
    switch (pt) {
      case DOCTOTEXT_PARSER_PDF:
        return "pdf";
      case DOCTOTEXT_PARSER_DOC:
        return "doc";
      case DOCTOTEXT_PARSER_XLS:
        return "xls";
      case DOCTOTEXT_PARSER_PPT:
        return "ppt";
      case DOCTOTEXT_PARSER_RTF:
        return "rtf";
      case DOCTOTEXT_PARSER_XLSB:
        return "xlsb";
      case DOCTOTEXT_PARSER_EML:
        return "eml";
      case DOCTOTEXT_PARSER_HTML:
        return "html";
      case DOCTOTEXT_PARSER_ODFXML:
        return "xml";
      case DOCTOTEXT_PARSER_ODF_OOXML:
        return "docx";
      case DOCTOTEXT_PARSER_IWORK:
        return "pages";
      case DOCTOTEXT_PARSER_TXT:
        return "txt";
      case DOCTOTEXT_PARSER_IMAGE:
        if (len >= 4 && std::memcmp(buf, "\x89PNG", 4) == 0) return "png";
        if (len >= 3 && static_cast<unsigned char>(buf[0]) == 0xFF && static_cast<unsigned char>(buf[1]) == 0xD8)
          return "jpg";
        if (len >= 12 && std::memcmp(buf, "RIFF", 4) == 0 && std::memcmp(buf + 8, "WEBP", 4) == 0) return "webp";
        if (len >= 2 && std::memcmp(buf, "BM", 2) == 0) return "bmp";
        if (len >= 4 && (std::memcmp(buf, "II*\x00", 4) == 0 || std::memcmp(buf, "MM\x00*", 4) == 0)) return "tiff";
        return "png";
      default:
        break;
    }
  }
  // Fallback quick ASCII text heuristic
  const size_t probe_len = std::min<size_t>(len, 4096);
  size_t nul_count = 0;
  for (size_t i = 0; i < probe_len; ++i) {
    if (buf[i] == '\0') ++nul_count;
  }
  if (nul_count == 0) {
    return "txt";
  }
  return "";
}

#ifndef _WIN32
// Keep POSIX libarchive header charset conversions in UTF-8 on the calling thread only
// without calling process-global setlocale().
struct ScopedThreadUtf8Locale {
  static locale_t CreateUtf8Locale() {
    locale_t loc = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
    return loc ? loc : newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
  }
  locale_t new_loc = CreateUtf8Locale();
  locale_t old_loc = new_loc ? uselocale(new_loc) : nullptr;
  ScopedThreadUtf8Locale() = default;
  ~ScopedThreadUtf8Locale() {
    if (old_loc) {
      uselocale(old_loc);
    }
    if (new_loc) {
      freelocale(new_loc);
    }
  }
  ScopedThreadUtf8Locale(const ScopedThreadUtf8Locale &) = delete;
  ScopedThreadUtf8Locale &operator=(const ScopedThreadUtf8Locale &) = delete;
};
#endif

struct ArchiveScope {
  archive *handle = nullptr;
  ~ArchiveScope() { Close(); }
  void Close() {
    if (handle) {
      archive_read_close(handle);
      archive_read_free(handle);
      handle = nullptr;
    }
  }
};

enum class MagicRoute { kAutoAll, kRar5, kRar4, k7z, kZip };

MagicRoute DetectMagicRoute(const unsigned char *head, size_t head_len) {
  static constexpr unsigned char kRar5Sig[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00};
  static constexpr unsigned char kRar4Sig[] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
  static constexpr unsigned char k7zSig[] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
  static constexpr unsigned char kZipSig[] = {0x50, 0x4B, 0x03, 0x04};
  static constexpr unsigned char kZipEmptySig[] = {0x50, 0x4B, 0x05, 0x06};

  if (HasMagic(head, head_len, kRar5Sig, sizeof(kRar5Sig))) return MagicRoute::kRar5;
  if (HasMagic(head, head_len, kRar4Sig, sizeof(kRar4Sig))) return MagicRoute::kRar4;
  if (HasMagic(head, head_len, k7zSig, sizeof(k7zSig))) return MagicRoute::k7z;
  if (HasMagic(head, head_len, kZipSig, sizeof(kZipSig)) ||
      HasMagic(head, head_len, kZipEmptySig, sizeof(kZipEmptySig))) {
    return MagicRoute::kZip;
  }
  return MagicRoute::kAutoAll;
}

void ApplyMagicRoute(archive *a, MagicRoute route, bool allow_raw) {
  switch (route) {
    case MagicRoute::kRar5:
      archive_read_support_format_rar5(a);
      return;
    case MagicRoute::kRar4:
      archive_read_support_format_rar(a);
      return;
    case MagicRoute::k7z:
      archive_read_support_format_7zip(a);
      return;
    case MagicRoute::kZip:
      archive_read_support_format_zip(a);
      return;
    case MagicRoute::kAutoAll:
      archive_read_support_filter_all(a);
      archive_read_support_format_all(a);
      if (allow_raw) {
        archive_read_support_format_raw(a);
      }
      return;
  }
}

int OpenArchiveHandle(archive *a, const std::string &file_path, const char *mem_ptr, size_t mem_len) {
  if (mem_ptr != nullptr) {
    return archive_read_open_memory(a, mem_ptr, mem_len);
  }
#ifdef _WIN32
  std::wstring wpath = common::EncodingUtils::Utf8ToWide(file_path);
  return archive_read_open_filename_w(a, wpath.c_str(), 65536);
#else
  return archive_read_open_filename(a, file_path.c_str(), 65536);
#endif
}

struct WalkState {
  const ArchiveExtractOptions &options;
  const ArchiveItemCallback &cb;
  size_t total_entries_seen = 0;
  std::chrono::steady_clock::time_point last_emit_time = std::chrono::steady_clock::now();
  bool cancelled = false;

  bool Emit(const ArchiveItem &item) {
    last_emit_time = std::chrono::steady_clock::now();
    if (!cb(item)) {
      cancelled = true;
      return false;
    }
    return true;
  }

  bool MaybeHeartbeat(const std::string &virtual_path) {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_emit_time).count() >= 1500) {
      ArchiveItem hb;
      hb.virtual_path = virtual_path;
      hb.event = ArchiveItemEvent::kHeartbeat;
      return Emit(hb);
    }
    return true;
  }
};

bool WalkContainer(const std::string &virtual_path, const std::string &container_ext, const std::string &file_path,
                   const char *mem_ptr, size_t mem_len, int depth, WalkState &state, std::string *out_error) {
  if (state.cancelled) return true;

  unsigned char head[16] = {};
  size_t head_len = 0;
  if (mem_ptr != nullptr) {
    head_len = std::min(mem_len, sizeof(head));
    if (head_len > 0) std::memcpy(head, mem_ptr, head_len);
  } else {
    auto ifs = common::EncodingUtils::OpenIfstream(file_path, std::ios::binary);
    if (!ifs.is_open()) {
      if (out_error) *out_error = "Failed to open archive file: " + file_path;
      return false;
    }
    ifs.read(reinterpret_cast<char *>(head), sizeof(head));
    head_len = static_cast<size_t>(ifs.gcount());
  }

  const bool allow_raw = IsSingleStreamCompressionExt(container_ext);
  MagicRoute route = DetectMagicRoute(head, head_len);

  ArchiveScope scope;
  scope.handle = archive_read_new();
  if (!scope.handle) {
    if (out_error) *out_error = "archive_read_new failed";
    return false;
  }
  ApplyMagicRoute(scope.handle, route, allow_raw);

  int open_rc = OpenArchiveHandle(scope.handle, file_path, mem_ptr, mem_len);
  if (open_rc != ARCHIVE_OK && route != MagicRoute::kAutoAll) {
    // Fallback to generic format_all if explicit magic route rejected SFX/unusual variant
    scope.Close();
    scope.handle = archive_read_new();
    if (!scope.handle) {
      if (out_error) *out_error = "archive_read_new fallback failed";
      return false;
    }
    ApplyMagicRoute(scope.handle, MagicRoute::kAutoAll, allow_raw);
    open_rc = OpenArchiveHandle(scope.handle, file_path, mem_ptr, mem_len);
  }

  if (open_rc != ARCHIVE_OK) {
    const char *err = archive_error_string(scope.handle);
    const std::string err_msg = err ? err : "Failed to open archive";
    ArchiveItem item;
    item.virtual_path = virtual_path;
    item.file_name = ExtractBasename(virtual_path);
    item.ext = container_ext;
    item.event = LooksLikeEncryptionError(err) ? ArchiveItemEvent::kEncrypted : ArchiveItemEvent::kFailed;
    item.message = err_msg;
    state.Emit(item);
    if (out_error && item.event == ArchiveItemEvent::kFailed) *out_error = err_msg;
    return item.event == ArchiveItemEvent::kEncrypted;
  }

  archive_entry *entry = nullptr;
  int rc = ARCHIVE_OK;
  std::vector<char> read_buf;
  while (!state.cancelled &&
         ((rc = archive_read_next_header(scope.handle, &entry)) == ARCHIVE_OK || rc == ARCHIVE_WARN)) {
    if (++state.total_entries_seen > state.options.max_entries) {
      LOG_WARN("[ArchiveReader] Reached max entry limit ({}) inside '{}'", state.options.max_entries, virtual_path);
      break;
    }

    const unsigned int ftype = archive_entry_filetype(entry);
    if (ftype != 0 && (ftype & S_IFMT) != S_IFREG) {
      archive_read_data_skip(scope.handle);
      if (!state.MaybeHeartbeat(virtual_path)) break;
      continue;
    }

    std::string rel_path = ExtractEntryRelPathUtf8(entry);
    std::string ext = ExtractExtLower(rel_path);
    const bool is_raw_nameless = (allow_raw && rel_path == "data" && ext.empty());
    if (is_raw_nameless) {
      const size_t last_dot = virtual_path.find_last_of('.');
      if (last_dot != std::string::npos) {
        ext = ExtractExtLower(virtual_path.substr(0, last_dot));
      }
      if (!ext.empty()) {
        rel_path = ExtractBasename(virtual_path.substr(0, last_dot));
      }
    }

    const std::string child_vpath = rel_path.empty() ? virtual_path : (virtual_path + "/" + rel_path);
    const std::string child_name = ExtractBasename(rel_path.empty() ? virtual_path : rel_path);
    const bool size_known = archive_entry_size_is_set(entry) != 0;
    const int64_t raw_entry_size = size_known ? archive_entry_size(entry) : 0;
    const uint64_t entry_size = (raw_entry_size > 0) ? static_cast<uint64_t>(raw_entry_size) : 0;
    const int64_t entry_mtime = archive_entry_mtime(entry);

    bool is_nested_arch = !ext.empty() && IsContainerOrArchiveExt(ext) && (depth < state.options.max_depth);
    const bool is_enabled_doc =
        !ext.empty() && state.options.enabled_exts.find(ext) != state.options.enabled_exts.end() && !is_nested_arch;

    if (!is_nested_arch && !is_enabled_doc && !is_raw_nameless) {
      archive_read_data_skip(scope.handle);
      if (!state.MaybeHeartbeat(child_vpath)) break;
      continue;
    }

    if (archive_entry_is_encrypted(entry)) {
      ArchiveItem enc_item;
      enc_item.virtual_path = child_vpath;
      enc_item.file_name = child_name;
      enc_item.ext = ext;
      enc_item.entry_size = entry_size;
      enc_item.entry_mtime = entry_mtime;
      enc_item.event = ArchiveItemEvent::kEncrypted;
      enc_item.message = "Archive entry is encrypted and requires a password";
      state.Emit(enc_item);
      archive_read_data_skip(scope.handle);
      continue;
    }

    const uint64_t read_limit = is_nested_arch ? state.options.max_nested_archive_bytes : state.options.max_entry_bytes;
    auto emit_skipped_too_large = [&](uint64_t observed_bytes, const char *reason) {
      LOG_INFO("[ArchiveReader] Skipping oversized archive entry '{}' ({} bytes > limit {} bytes): {}", child_vpath,
               observed_bytes, read_limit, reason);
      ArchiveItem big_item;
      big_item.virtual_path = child_vpath;
      big_item.file_name = child_name;
      big_item.ext = ext;
      big_item.entry_size = observed_bytes;
      big_item.entry_mtime = entry_mtime;
      big_item.event = ArchiveItemEvent::kSkippedTooLarge;
      big_item.message = reason;
      state.Emit(big_item);
      archive_read_data_skip(scope.handle);
    };

    if (!is_nested_arch && size_known && entry_size > read_limit) {
      emit_skipped_too_large(entry_size, "Archive entry exceeds max_file_size");
      continue;
    }

    constexpr size_t kMaxPreReserveBytes = 4ULL * 1024ULL * 1024ULL;
    read_buf.clear();
    if (size_known && entry_size > 0 && entry_size <= read_limit) {
      read_buf.reserve(static_cast<size_t>(std::min<uint64_t>(entry_size, kMaxPreReserveBytes)));
    }

    char chunk[65536];
    la_ssize_t n = 0;
    bool exceeded_limit = false;
    while ((n = archive_read_data(scope.handle, chunk, sizeof(chunk))) > 0) {
      if (read_buf.size() + static_cast<size_t>(n) > read_limit) {
        exceeded_limit = true;
        break;
      }
      read_buf.insert(read_buf.end(), chunk, chunk + n);
    }

    if (exceeded_limit) {
      emit_skipped_too_large(read_buf.size() + static_cast<size_t>(n), "Decompressed stream exceeds size limit");
      if (read_buf.capacity() > kMaxPreReserveBytes) {
        std::vector<char>().swap(read_buf);
      }
      continue;
    }

    if (n < 0) {
      const char *err = archive_error_string(scope.handle);
      ArchiveItem err_item;
      err_item.virtual_path = child_vpath;
      err_item.file_name = child_name;
      err_item.ext = ext;
      err_item.entry_size = entry_size;
      err_item.entry_mtime = entry_mtime;
      err_item.event = LooksLikeEncryptionError(err) ? ArchiveItemEvent::kEncrypted : ArchiveItemEvent::kFailed;
      err_item.message = err ? err : "Failed to decompress archive entry";
      state.Emit(err_item);
      continue;
    }

    // For raw unnamed streams (e.g. .bz2/.lzma), sniff whether payload is a nested archive or a target document
    if (is_raw_nameless && ext.empty() && !read_buf.empty()) {
      std::string sniffed_arch_ext;
      if (depth < state.options.max_depth &&
          IsArchiveMagicBytes(reinterpret_cast<const unsigned char *>(read_buf.data()), read_buf.size(),
                              &sniffed_arch_ext)) {
        is_nested_arch = true;
        ext = sniffed_arch_ext;
      } else {
        ext = SniffExtensionFromBuffer(read_buf.data(), read_buf.size());
        if (ext.empty() || state.options.enabled_exts.find(ext) == state.options.enabled_exts.end()) {
          if (!state.MaybeHeartbeat(child_vpath)) break;
          continue;
        }
      }
    }

    if (is_nested_arch) {
      std::string nested_err;
      WalkContainer(child_vpath, ext, "", read_buf.data(), read_buf.size(), depth + 1, state, &nested_err);
      if (read_buf.capacity() > kMaxPreReserveBytes) {
        std::vector<char>().swap(read_buf);
      }
      continue;
    }

    ArchiveItem doc_item;
    doc_item.virtual_path = child_vpath;
    doc_item.file_name = child_name;
    doc_item.ext = ext;
    doc_item.entry_size = read_buf.size();
    doc_item.entry_mtime = entry_mtime;
    doc_item.event = ArchiveItemEvent::kData;
    doc_item.raw_data = read_buf.data();
    doc_item.raw_size = read_buf.size();
    const bool keep_going = state.Emit(doc_item);
    if (read_buf.capacity() > kMaxPreReserveBytes) {
      std::vector<char>().swap(read_buf);
    }
    if (!keep_going) {
      break;
    }
  }

  if (!state.cancelled && rc != ARCHIVE_EOF) {
    const char *err = archive_error_string(scope.handle);
    const std::string err_msg = err ? err : "Archive stream terminated abnormally";
    ArchiveItem err_item;
    err_item.virtual_path = virtual_path;
    err_item.file_name = ExtractBasename(virtual_path);
    err_item.ext = container_ext;
    err_item.event = LooksLikeEncryptionError(err) ? ArchiveItemEvent::kEncrypted : ArchiveItemEvent::kFailed;
    err_item.message = err_msg;
    state.Emit(err_item);
    if (out_error && err_item.event == ArchiveItemEvent::kFailed) *out_error = err_msg;
    return err_item.event == ArchiveItemEvent::kEncrypted;
  }

  return true;
}

}  // namespace

bool ArchiveReader::Extract(const std::string &archive_path, const ArchiveExtractOptions &options,
                            const ArchiveItemCallback &cb, std::string *out_error) const {
  if (archive_path.empty() || !cb) {
    if (out_error) *out_error = "Invalid arguments to ArchiveReader::Extract";
    return false;
  }
#ifndef _WIN32
  ScopedThreadUtf8Locale utf8_locale_guard;
#endif
  WalkState state{options, cb};
  const std::string ext = ExtractExtLower(archive_path);
  return WalkContainer(archive_path, ext, archive_path, nullptr, 0, 0, state, out_error);
}

}  // namespace worker
}  // namespace soeasy
