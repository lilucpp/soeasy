#ifndef SOEASY_SRC_WORKER_L1_TEXT_PARSER_H_
#define SOEASY_SRC_WORKER_L1_TEXT_PARSER_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace soeasy {
namespace worker {

class L1TextParser {
 public:
  L1TextParser();
  ~L1TextParser();

  // Disallow copy and assign
  L1TextParser(const L1TextParser&) = delete;
  L1TextParser& operator=(const L1TextParser&) = delete;

  /**
   * @brief Parse a text or code document from raw bytes into normalized UTF-8 plain text.
   *
   * Automatically detects source encoding (GB18030/GBK/UTF-16/Big5/etc.) using uchardet,
   * converts to UTF-8 via iconv, strips HTML/XML/RTF tags (if applicable), and cleans
   * non-printable control characters.
   *
   * @param raw_bytes Raw document bytes
   * @param raw_size Number of bytes
   * @param ext File extension in lowercase without dot (e.g. "txt", "cpp", "html", "md")
   * @param out_utf8_text Receives converted and normalized UTF-8 string
   * @return true on success, false on fatal decoding failure
   */
  bool Parse(const char* raw_bytes, size_t raw_size, const std::string& ext,
             std::string* out_utf8_text);

  /**
   * @brief Helper to parse a file on disk directly.
   */
  bool ParseFile(const std::string& file_path, const std::string& ext,
                 std::string* out_utf8_text);

  /**
   * @brief Detect character encoding name of raw byte stream using uchardet.
   *
   * @param raw_bytes Raw byte buffer
   * @param raw_size Buffer size
   * @return Detected encoding string (e.g. "UTF-8", "GB18030", "UTF-16LE")
   */
  static std::string DetectEncoding(const char* raw_bytes, size_t raw_size);

 private:
  static bool ConvertToUtf8(const char* in_bytes, size_t in_size,
                            const std::string& from_encoding,
                            std::string* out_utf8);
  static void CleanControlChars(std::string* text);
  static void StripHtmlTags(std::string* text);
  static void StripRtfTags(std::string* text);
};

}  // namespace worker
}  // namespace soeasy

#endif  // SOEASY_SRC_WORKER_L1_TEXT_PARSER_H_
