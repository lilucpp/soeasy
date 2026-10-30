// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_WORKER_L2_OFFICE_PARSER_H_
#define SOEASY_SRC_WORKER_L2_OFFICE_PARSER_H_

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace soeasy {
namespace worker {

class L2OfficeParser {
 public:
  L2OfficeParser();
  ~L2OfficeParser() = default;

  // Disallow copy and assign
  L2OfficeParser(const L2OfficeParser&) = delete;
  L2OfficeParser& operator=(const L2OfficeParser&) = delete;

  /**
   * @brief Check if this extension is handled by L2OfficeParser.
   *
   * @param ext File extension in lowercase without dot (e.g. "docx", "pdf", "xlsx")
   */
  bool IsSupported(const std::string& ext) const;

  /**
   * @brief Parse an office document from file into normalized UTF-8 plain text.
   *
   * @param file_path Canonical filesystem path
   * @param ext Extension in lowercase
   * @param out_utf8_text Output extracted text
   * @param is_scanned_pdf Optional flag indicating scanned / image-only PDF
   * @return true on success
   */
  bool Parse(const std::string& file_path, const std::string& ext,
             std::string* out_utf8_text, bool* is_scanned_pdf = nullptr);

  /**
   * @brief Configure DocToText logging. Default disabled. When enabled, verbose logs output to log_file_path.
   *
   * @param enable Whether DocToText detailed logging is enabled
   * @param log_file_path Path to the log file to append DocToText output
   */
  static void SetLogging(bool enable, const std::string& log_file_path = "");

  /**
   * @brief Configure whether embedded images in Office / PDF documents are extracted and OCR'd.
   * Default is false (disabled for performance).
   *
   * @param enable Whether embedded image OCR is enabled
   */
  static void SetEmbeddedImageOcr(bool enable);
  static bool embedded_image_ocr();

  // Document format extractors
  static bool ParseDocx(const std::string& file_path, std::string* out_utf8);
  static bool ParseXlsx(const std::string& file_path, std::string* out_utf8);
  static bool ParsePptx(const std::string& file_path, std::string* out_utf8);
  static bool ParsePdf(const std::string& file_path, std::string* out_utf8,
                       bool* is_scanned);
  static bool ParseLegacyOffice(const std::string& file_path,
                                const std::string& ext, std::string* out_utf8);

  /**
   * @brief Extract clean text content from OOXML XML markup.
   */
  static std::string ExtractXmlText(const std::string& xml_content,
                                    bool insert_newlines = true);

  /**
   * @brief Unescape standard XML entities (&amp;, &lt;, &gt;, &quot;, &apos;).
   */
  static std::string UnescapeXmlEntities(const std::string& text);

 private:
  std::unordered_set<std::string> supported_exts_;
};

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_L2_OFFICE_PARSER_H_
