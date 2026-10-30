#include "worker/l1_text_parser.h"

#include <iconv.h>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "uchardet/uchardet.h"
#include "utf8.h"

namespace soeasy {
namespace worker {

L1TextParser::L1TextParser() = default;
L1TextParser::~L1TextParser() = default;

std::string L1TextParser::DetectEncoding(const char* raw_bytes, size_t raw_size) {
  if (!raw_bytes || raw_size == 0) {
    return "UTF-8";
  }

  // Fast check: BOM detection
  if (raw_size >= 3 &&
      static_cast<uint8_t>(raw_bytes[0]) == 0xEF &&
      static_cast<uint8_t>(raw_bytes[1]) == 0xBB &&
      static_cast<uint8_t>(raw_bytes[2]) == 0xBF) {
    return "UTF-8";
  }
  if (raw_size >= 2 &&
      static_cast<uint8_t>(raw_bytes[0]) == 0xFF &&
      static_cast<uint8_t>(raw_bytes[1]) == 0xFE) {
    return "UTF-16LE";
  }
  if (raw_size >= 2 &&
      static_cast<uint8_t>(raw_bytes[0]) == 0xFE &&
      static_cast<uint8_t>(raw_bytes[1]) == 0xFF) {
    return "UTF-16BE";
  }

  uchardet_t handle = uchardet_new();
  if (!handle) {
    return "UTF-8";
  }

  // Probe up to first 64KB for speed and accuracy
  size_t probe_len = std::min<size_t>(raw_size, 65536);
  uchardet_handle_data(handle, raw_bytes, probe_len);
  uchardet_data_end(handle);

  const char* enc = uchardet_get_charset(handle);
  std::string result = (enc && enc[0] != '\0') ? enc : "UTF-8";
  uchardet_delete(handle);

  // Normalize encoding names to standard uppercase
  std::transform(result.begin(), result.end(), result.begin(), ::toupper);
  return result;
}

bool L1TextParser::ConvertToUtf8(const char* in_bytes, size_t in_size,
                                 const std::string& from_encoding,
                                 std::string* out_utf8) {
  if (!out_utf8) {
    return false;
  }
  if (in_size == 0) {
    out_utf8->clear();
    return true;
  }

  iconv_t cd = iconv_open("UTF-8//IGNORE", from_encoding.c_str());
  if (cd == reinterpret_cast<iconv_t>(-1)) {
    // Fallback: try without //IGNORE
    cd = iconv_open("UTF-8", from_encoding.c_str());
    if (cd == reinterpret_cast<iconv_t>(-1)) {
      return false;
    }
  }

  size_t in_left = in_size;
  size_t out_capacity = in_size * 4 + 64;
  std::vector<char> out_buf(out_capacity);
  char* out_ptr = out_buf.data();
  size_t out_left = out_capacity;

#ifdef _WIN32
  const char* in_ptr = in_bytes;
  size_t res = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
#else
  char* in_ptr = const_cast<char*>(in_bytes);
  size_t res = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
#endif
  iconv_close(cd);

  if (res == static_cast<size_t>(-1) && out_left == out_capacity) {
    return false;
  }

  size_t written = out_capacity - out_left;
  out_utf8->assign(out_buf.data(), written);
  return true;
}

void L1TextParser::CleanControlChars(std::string* text) {
  if (!text || text->empty()) {
    return;
  }

  std::string clean;
  clean.reserve(text->size());

  for (char c : *text) {
    unsigned char uc = static_cast<unsigned char>(c);
    // Retain newline, carriage return, and tab
    if (uc == '\n' || uc == '\r' || uc == '\t') {
      clean.push_back(c);
    } else if (uc < 0x20 || uc == 0x7F) {
      // Skip non-printable control characters
      continue;
    } else {
      clean.push_back(c);
    }
  }

  *text = std::move(clean);
}

void L1TextParser::StripHtmlTags(std::string* text) {
  if (!text || text->empty()) {
    return;
  }

  std::string plain;
  plain.reserve(text->size());
  bool inside_tag = false;

  for (size_t i = 0; i < text->size(); ++i) {
    char c = (*text)[i];
    if (c == '<') {
      inside_tag = true;
    } else if (c == '>') {
      inside_tag = false;
      plain.push_back(' ');  // Space separator for block elements
    } else if (!inside_tag) {
      plain.push_back(c);
    }
  }

  *text = std::move(plain);
}

void L1TextParser::StripRtfTags(std::string* text) {
  if (!text || text->empty()) {
    return;
  }

  std::string plain;
  plain.reserve(text->size());
  bool in_control = false;

  for (size_t i = 0; i < text->size(); ++i) {
    char c = (*text)[i];
    if (c == '{' || c == '}') {
      continue;
    }
    if (c == '\\') {
      in_control = true;
      continue;
    }
    if (in_control) {
      if (std::isspace(static_cast<unsigned char>(c)) || c == ';' || c == '\n') {
        in_control = false;
      }
      continue;
    }
    plain.push_back(c);
  }

  *text = std::move(plain);
}

bool L1TextParser::Parse(const char* raw_bytes, size_t raw_size,
                         const std::string& ext, std::string* out_utf8_text) {
  if (!out_utf8_text) {
    return false;
  }
  if (!raw_bytes || raw_size == 0) {
    out_utf8_text->clear();
    return true;
  }

  // 1. Detect encoding
  std::string encoding = DetectEncoding(raw_bytes, raw_size);

  // Skip UTF-8 BOM if present
  const char* content_start = raw_bytes;
  size_t content_size = raw_size;
  if (raw_size >= 3 &&
      static_cast<uint8_t>(raw_bytes[0]) == 0xEF &&
      static_cast<uint8_t>(raw_bytes[1]) == 0xBB &&
      static_cast<uint8_t>(raw_bytes[2]) == 0xBF) {
    content_start += 3;
    content_size -= 3;
    encoding = "UTF-8";
  }

  // 2. Convert to UTF-8
  std::string utf8_content;
  if (encoding == "UTF-8" || encoding == "ASCII" || encoding == "US-ASCII") {
    utf8_content.assign(content_start, content_size);
  } else {
    if (!ConvertToUtf8(content_start, content_size, encoding, &utf8_content)) {
      // Fallback: copy as-is and sanitize
      utf8_content.assign(content_start, content_size);
    }
  }

  // 3. Ensure valid UTF-8 sequence
  if (!utf8::is_valid(utf8_content.begin(), utf8_content.end())) {
    std::string sanitized;
    utf8::replace_invalid(utf8_content.begin(), utf8_content.end(),
                          std::back_inserter(sanitized));
    utf8_content = std::move(sanitized);
  }

  // 4. Strip markup tags if applicable
  if (ext == "html" || ext == "htm" || ext == "xml") {
    StripHtmlTags(&utf8_content);
  } else if (ext == "rtf") {
    StripRtfTags(&utf8_content);
  }

  // 5. Clean non-printable control characters
  CleanControlChars(&utf8_content);

  *out_utf8_text = std::move(utf8_content);
  return true;
}

bool L1TextParser::ParseFile(const std::string& file_path, const std::string& ext,
                             std::string* out_utf8_text) {
  if (!out_utf8_text) {
    return false;
  }

  std::ifstream ifs(file_path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }

  std::string buffer((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
  return Parse(buffer.data(), buffer.size(), ext, out_utf8_text);
}

}  // namespace worker
}  // namespace soeasy
