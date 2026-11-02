#include "service/doc_prechecker.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#include "common/encoding_utils.h"

namespace soeasy {
namespace service {

PrecheckResult DocPrechecker::Check(const std::string& file_path,
                                    const std::string& ext,
                                    uint64_t file_size) {
  PrecheckResult res;

  if (file_size == 0) {
    res.status = PrecheckStatus::kEmpty;
    res.reason = "Empty file (0 bytes)";
    return res;
  }

  std::string lower_ext = ext;
  std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);

  // 1. Text and source code formats: lightning-fast 5s timeout
  if (lower_ext == "txt" || lower_ext == "md" || lower_ext == "json" ||
      lower_ext == "csv" || lower_ext == "xml" || lower_ext == "html" ||
      lower_ext == "cpp" || lower_ext == "h" || lower_ext == "c" ||
      lower_ext == "py" || lower_ext == "java" || lower_ext == "sh" ||
      lower_ext == "log") {
    res.recommended_timeout_ms = 5000;
    return res;
  }

  // 2. Pure image formats: 120s timeout for heavy OCR
  if (lower_ext == "jpg" || lower_ext == "jpeg" || lower_ext == "png" ||
      lower_ext == "bmp" || lower_ext == "tiff" || lower_ext == "tif" ||
      lower_ext == "webp" || lower_ext == "gif" || lower_ext == "svg" ||
      lower_ext == "pnm" || lower_ext == "tga") {
    res.has_images = true;
    res.recommended_timeout_ms = 120000;
    return res;
  }

  // 3. Minimum size check for complex office/PDF documents
  // Valid DOCX, XLSX, PPTX, PDF, or OLE2 DOC files contain directory and metadata trees.
  // Files smaller than 1024 bytes are truncated/corrupt.
  if ((lower_ext == "docx" || lower_ext == "xlsx" || lower_ext == "pptx" ||
       lower_ext == "pdf" || lower_ext == "doc" || lower_ext == "xls" ||
       lower_ext == "ppt" || lower_ext == "odt" || lower_ext == "ods" ||
       lower_ext == "odp") && file_size < 1024) {
    res.status = PrecheckStatus::kTruncatedOrCorrupt;
    res.reason = "File size < 1KB (truncated complex document)";
    return res;
  }

  auto file = common::EncodingUtils::OpenIfstream(file_path, std::ios::binary);
  if (!file.is_open()) {
    res.status = PrecheckStatus::kTruncatedOrCorrupt;
    res.reason = "Cannot open file";
    return res;
  }

  // Read header (up to first 512 bytes)
  std::vector<uint8_t> header(std::min<size_t>(file_size, 512));
  file.read(reinterpret_cast<char*>(header.data()), header.size());

  // 4. OOXML Documents (docx, xlsx, pptx)
  if (lower_ext == "docx" || lower_ext == "xlsx" || lower_ext == "pptx") {
    // Check for OLE2 Compound File header (D0 CF 11 E0 A1 B1 1A E1)
    // In OOXML, an OLE2 container indicates password protection (EncryptedPackage).
    const uint8_t kOle2Magic[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (header.size() >= 8 && std::memcmp(header.data(), kOle2Magic, 8) == 0) {
      res.status = PrecheckStatus::kEncrypted;
      res.reason = "Password-encrypted OOXML (OLE2 EncryptedPackage)";
      return res;
    }

    // Must start with ZIP Local File Header (PK\x03\x04)
    const uint8_t kZipLocalHeader[] = {0x50, 0x4B, 0x03, 0x04};
    if (header.size() < 4 || std::memcmp(header.data(), kZipLocalHeader, 4) != 0) {
      res.status = PrecheckStatus::kDisguised;
      res.reason = "Invalid ZIP magic header (disguised or corrupt)";
      return res;
    }

    // Check tail for ZIP End of Central Directory (EOCD: PK\x05\x06)
    size_t tail_len = std::min<size_t>(file_size, 65536);
    std::vector<uint8_t> tail(tail_len);
    file.seekg(-static_cast<std::streamoff>(tail_len), std::ios::end);
    file.read(reinterpret_cast<char*>(tail.data()), tail_len);

    const uint8_t kZipEOCD[] = {0x50, 0x4B, 0x05, 0x06};
    auto it = std::search(tail.begin(), tail.end(), kZipEOCD, kZipEOCD + 4);
    if (it == tail.end()) {
      res.status = PrecheckStatus::kTruncatedOrCorrupt;
      res.reason = "Missing ZIP EOCD record (truncated ZIP archive)";
      return res;
    }

    // Check if document contains embedded images (look for "media/" in ZIP directory)
    const char kMediaPattern[] = "media/";
    auto it_media = std::search(tail.begin(), tail.end(), kMediaPattern, kMediaPattern + 6);
    if (it_media != tail.end()) {
      res.has_images = true;
      res.recommended_timeout_ms = 60000;  // 60s for document with embedded images
    } else {
      res.has_images = false;
      res.recommended_timeout_ms = 15000;  // 15s for text-only office document
    }
    return res;
  }

  // 5. Binary Office Documents (doc, xls, ppt)
  if (lower_ext == "doc" || lower_ext == "xls" || lower_ext == "ppt") {
    const uint8_t kOle2Magic[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (header.size() < 8 || std::memcmp(header.data(), kOle2Magic, 8) != 0) {
      res.status = PrecheckStatus::kDisguised;
      res.reason = "Not a valid OLE2 compound file";
      return res;
    }

    // Inspect OLE2 Directory sectors to check for encryption streams & record flags
    if (header.size() >= 512) {
      uint16_t sector_shift = *reinterpret_cast<const uint16_t*>(header.data() + 0x1E);
      if (sector_shift < 9 || sector_shift > 12) sector_shift = 9;  // standard 512
      size_t sector_size = 1ULL << sector_shift;

      uint32_t sect_dir_start = *reinterpret_cast<const uint32_t*>(header.data() + 0x30);
      if (sect_dir_start != 0xFFFFFFFE && sect_dir_start != 0xFFFFFFFF) {
        size_t dir_offset = (static_cast<uint64_t>(sect_dir_start) + 1) * sector_size;
        file.seekg(dir_offset, std::ios::beg);
        std::vector<uint8_t> dir_sector(sector_size);
        file.read(reinterpret_cast<char*>(dir_sector.data()), dir_sector.size());
        size_t num_entries = file.gcount() / 128;

        for (size_t i = 0; i < num_entries; ++i) {
          const uint8_t* entry = dir_sector.data() + i * 128;
          uint16_t name_len = *reinterpret_cast<const uint16_t*>(entry + 0x40);
          if (name_len == 0 || name_len > 64) continue;

          const char16_t* u16_name = reinterpret_cast<const char16_t*>(entry);
          size_t char_count = (name_len / 2) - 1;
          std::u16string name(u16_name, char_count);

          if (name == u"EncryptedPackage" || name == u"EncryptionInfo" ||
              name == u"EncryptedSummary") {
            res.status = PrecheckStatus::kEncrypted;
            res.reason = "OLE2 directory contains encrypted stream: " +
                         std::string(name == u"EncryptedPackage"   ? "EncryptedPackage"
                                     : name == u"EncryptionInfo" ? "EncryptionInfo"
                                                                 : "EncryptedSummary");
            return res;
          }

          uint32_t sect_start = *reinterpret_cast<const uint32_t*>(entry + 116);
          uint64_t stream_size = *reinterpret_cast<const uint64_t*>(entry + 120);
          if (sect_start == 0xFFFFFFFE || sect_start == 0xFFFFFFFF) continue;

          uint64_t stream_offset = (static_cast<uint64_t>(sect_start) + 1) * sector_size;

          // WordDocument: check FIB fEncrypted flag at offset 0x0A
          if (lower_ext == "doc" && name == u"WordDocument" && stream_size >= 12) {
            file.seekg(stream_offset, std::ios::beg);
            uint8_t fib[12];
            file.read(reinterpret_cast<char*>(fib), sizeof(fib));
            if (file.gcount() == 12) {
              uint16_t fib_flags = *reinterpret_cast<uint16_t*>(fib + 10);
              if ((fib_flags & 0x0100) != 0) {
                res.status = PrecheckStatus::kEncrypted;
                res.reason = "MS Word FIB flag indicates password encryption (fEncrypted)";
                return res;
              }
            }
          }

          // Workbook: check for FILEPASS record (0x002F) or PROTECT (0x0086)
          if (lower_ext == "xls" && (name == u"Workbook" || name == u"Book") && stream_size >= 4) {
            file.seekg(stream_offset, std::ios::beg);
            std::vector<uint8_t> wb_buf(std::min<size_t>(stream_size, 4096));
            file.read(reinterpret_cast<char*>(wb_buf.data()), wb_buf.size());
            size_t bytes_read = file.gcount();
            size_t pos = 0;
            while (pos + 4 <= bytes_read) {
              uint16_t rtype = *reinterpret_cast<uint16_t*>(wb_buf.data() + pos);
              uint16_t rlen = *reinterpret_cast<uint16_t*>(wb_buf.data() + pos + 2);
              if (rtype == 0x002F) {
                res.status = PrecheckStatus::kEncrypted;
                res.reason = "MS Excel contains FILEPASS record (0x002F)";
                return res;
              }
              pos += 4 + rlen;
              if (rlen == 0) break;
            }
          }

          // PowerPoint Document: check for CryptSession10Container (0x2F14)
          if (lower_ext == "ppt" && name == u"PowerPoint Document" && stream_size >= 4) {
            file.seekg(stream_offset, std::ios::beg);
            std::vector<uint8_t> ppt_buf(std::min<size_t>(stream_size, 4096));
            file.read(reinterpret_cast<char*>(ppt_buf.data()), ppt_buf.size());
            size_t ppt_bytes = static_cast<size_t>(file.gcount());
            for (size_t p = 0; p + 2 <= ppt_bytes; ++p) {
              if (*reinterpret_cast<uint16_t*>(ppt_buf.data() + p) == 0x2F14) {
                res.status = PrecheckStatus::kEncrypted;
                res.reason = "MS PowerPoint contains CryptSession10Container record (0x2F14)";
                return res;
              }
            }
          }
        }
      }
    }

    res.recommended_timeout_ms = 15000;
    return res;
  }

  // 6. PDF Documents
  if (lower_ext == "pdf") {
    const uint8_t kPdfMagic[] = {'%', 'P', 'D', 'F', '-'};
    auto it_pdf = std::search(header.begin(), header.end(), kPdfMagic, kPdfMagic + 5);
    if (it_pdf == header.end()) {
      res.status = PrecheckStatus::kDisguised;
      res.reason = "Missing %PDF- header magic";
      return res;
    }

    size_t tail_len = std::min<size_t>(file_size, 4096);
    std::vector<uint8_t> tail(tail_len);
    file.seekg(-static_cast<std::streamoff>(tail_len), std::ios::end);
    file.read(reinterpret_cast<char*>(tail.data()), tail_len);

    const uint8_t kEofMagic[] = {'%', '%', 'E', 'O', 'F'};
    auto it_eof = std::search(tail.begin(), tail.end(), kEofMagic, kEofMagic + 5);
    if (it_eof == tail.end()) {
      res.status = PrecheckStatus::kTruncatedOrCorrupt;
      res.reason = "Missing %%EOF in tail (truncated PDF)";
      return res;
    }

    const uint8_t kEncryptPattern[] = {'/', 'E', 'n', 'c', 'r', 'y', 'p', 't'};
    auto it_enc = std::search(tail.begin(), tail.end(), kEncryptPattern, kEncryptPattern + 8);
    if (it_enc != tail.end()) {
      res.status = PrecheckStatus::kEncrypted;
      res.reason = "Password-protected PDF (contains /Encrypt dictionary)";
      return res;
    }

    res.recommended_timeout_ms = 15000;
    return res;
  }

  return res;
}

}  // namespace service
}  // namespace soeasy
