// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/l2_office_parser.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#include <direct.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "common/encoding_utils.h"
#include "doctotext_c_api.h"
#include "miniz.h"
#include "utf8.h"
#include "worker/ocr/image_filter.h"
#include "worker/ocr/ocr_engine_factory.h"

namespace soeasy {
namespace worker {

#ifndef _WIN32
static const char* kDevNull = "/dev/null";
#endif

namespace {

class NullStreamBuf : public std::streambuf {
 public:
  int overflow(int c) override { return c; }
};

static NullStreamBuf g_null_buf;
static std::ostream g_null_stream(&g_null_buf);

static std::mutex g_dt_mutex;
static bool g_dt_logging_enabled = false;
static std::string g_dt_log_path = "";
static std::unique_ptr<std::ofstream> g_dt_file_stream;
static std::atomic<bool> s_embedded_image_ocr{false};

uint64_t GetFileSize(const std::string& path) {
  uint64_t size = 0;
  if (common::EncodingUtils::GetFileStat(path, &size, nullptr)) {
    return size;
  }
  return 0;
}

#ifndef _WIN32
std::string DocToTextImageParserCallback(
    const std::string& image_name,
    const char* image_buffer,
    size_t image_size,
    void* user_data) {
  if (!image_buffer || image_size == 0) return "";
  try {
    std::vector<uint8_t> buf(
        reinterpret_cast<const uint8_t*>(image_buffer),
        reinterpret_cast<const uint8_t*>(image_buffer) + image_size);
    cv::Mat img = cv::imdecode(buf, cv::IMREAD_COLOR);
    if (img.empty()) return "";

    ocr::ImageFilter filter;
    if (filter.CheckDecodedImage(img) != ocr::FilterDecision::kPass) {
      return "";
    }

    static std::mutex s_ocr_mutex;
    static std::unique_ptr<ocr::IOcrEngine> s_ocr_engine;
    std::lock_guard<std::mutex> lock(s_ocr_mutex);
    if (!s_ocr_engine) {
      s_ocr_engine = ocr::OcrEngineFactory::CreateDefaultEngine();
    }
    if (!s_ocr_engine) return "";

    auto ocr_res = s_ocr_engine->Recognize(img);
    if (ocr_res.success && !ocr_res.full_text.empty()) {
      return ocr_res.full_text;
    }
  } catch (...) {
  }
  return "";
}

const char* DocToTextImageParserCallbackC(
    const char* image_name,
    const char* image_buffer,
    size_t image_size,
    void* user_data) {
  static thread_local std::string tls_ocr_result;
  tls_ocr_result = DocToTextImageParserCallback(
      image_name ? image_name : "",
      image_buffer,
      image_size,
      user_data);
  return tls_ocr_result.empty() ? nullptr : tls_ocr_result.c_str();
}
#endif

}  // namespace

L2OfficeParser::L2OfficeParser() {
  supported_exts_ = {
      "docx", "xlsx", "pptx", "pdf", "doc", "xls", "ppt", "rtf",
      "odt", "ods", "odp", "odg", "fodt", "fods", "fodp", "xlsb",
      "eml", "pages", "numbers", "key", "keynote", "wps", "et", "dps"
  };
}

bool L2OfficeParser::IsSupported(const std::string& ext) const {
  std::string lower = ext;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  return supported_exts_.find(lower) != supported_exts_.end();
}

void L2OfficeParser::SetLogging(bool enable, const std::string& log_file_path) {
  std::lock_guard<std::mutex> lock(g_dt_mutex);
  g_dt_logging_enabled = enable;
  if (!log_file_path.empty()) {
    g_dt_log_path = log_file_path;
  }
  if (g_dt_file_stream && g_dt_file_stream->is_open()) {
    g_dt_file_stream->close();
  }
  g_dt_file_stream.reset();

  if (g_dt_logging_enabled && !g_dt_log_path.empty()) {
    try {
      std::filesystem::path p(g_dt_log_path);
      if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
      }
      g_dt_file_stream = std::make_unique<std::ofstream>(g_dt_log_path, std::ios::app);
      if (g_dt_file_stream->is_open()) {
        *g_dt_file_stream << "\n========================================================\n"
                          << "[DocToText Session Started]\n"
                          << "========================================================\n"
                          << std::flush;
      }
    } catch (...) {
      g_dt_file_stream.reset();
    }
  }
}

void L2OfficeParser::SetEmbeddedImageOcr(bool enable) {
  s_embedded_image_ocr.store(enable, std::memory_order_relaxed);
}

bool L2OfficeParser::embedded_image_ocr() {
  return s_embedded_image_ocr.load(std::memory_order_relaxed);
}

bool L2OfficeParser::Parse(const std::string& file_path, const std::string& ext,
                           std::string* out_utf8_text, bool* is_scanned_pdf) {
  if (!out_utf8_text) return false;
  out_utf8_text->clear();
  if (is_scanned_pdf) *is_scanned_pdf = false;

  std::string lower = ext;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

#ifndef _WIN32
  // 1. Primary: Use DocToText (lithiumFlower/doctotext) for comprehensive multi-format extraction
  try {
    bool enable_logging = false;
    std::string log_file_path;
    {
      std::lock_guard<std::mutex> lock(g_dt_mutex);
      if (g_dt_logging_enabled && g_dt_file_stream && g_dt_file_stream->is_open()) {
        enable_logging = true;
        log_file_path = g_dt_log_path;
        *g_dt_file_stream << "\n[DocToText] Parsing: " << file_path << " (ext: " << lower << ")\n"
                          << std::flush;
      }
    }

    int ptype = DOCTOTEXT_PARSER_AUTO;
    if (lower == "docx" || lower == "xlsx" || lower == "pptx" || lower == "odt" || lower == "ods" || lower == "odp" || lower == "odg") {
      ptype = DOCTOTEXT_PARSER_ODF_OOXML;
    } else if (lower == "doc") {
      ptype = DOCTOTEXT_PARSER_DOC;
    } else if (lower == "xls") {
      ptype = DOCTOTEXT_PARSER_XLS;
    } else if (lower == "ppt") {
      ptype = DOCTOTEXT_PARSER_PPT;
    } else if (lower == "rtf") {
      ptype = DOCTOTEXT_PARSER_RTF;
    } else if (lower == "pdf") {
      ptype = DOCTOTEXT_PARSER_PDF;
    } else if (lower == "xlsb") {
      ptype = DOCTOTEXT_PARSER_XLSB;
    } else if (lower == "eml") {
      ptype = DOCTOTEXT_PARSER_EML;
    } else if (lower == "fodt" || lower == "fods" || lower == "fodp") {
      ptype = DOCTOTEXT_PARSER_ODFXML;
    } else if (lower == "pages" || lower == "numbers" || lower == "key" || lower == "keynote") {
      ptype = DOCTOTEXT_PARSER_IWORK;
    }

    DocToTextExtractorParams* params = doctotext_create_extractor_params();
    if (params) {
      doctotext_extractor_params_set_verbose_logging(params, enable_logging ? 1 : 0);
      std::string tmp_log_path;
      if (enable_logging && !log_file_path.empty()) {
        tmp_log_path = log_file_path + ".dt.tmp";
        doctotext_extractor_params_set_log_file(params, tmp_log_path.c_str());
      } else {
        doctotext_extractor_params_set_log_file(params, kDevNull);
      }
      doctotext_extractor_params_set_parser_type(params, ptype);
      if (s_embedded_image_ocr.load(std::memory_order_relaxed)) {
        doctotext_extractor_params_set_image_parser_callback(params, &DocToTextImageParserCallbackC, nullptr);
      } else {
        doctotext_extractor_params_set_image_parser_callback(params, nullptr, nullptr);
      }

      DocToTextException* exc = nullptr;
      DocToTextExtractedData* data = doctotext_process_file(file_path.c_str(), params, &exc);
      if (data) {
        const char* txt = doctotext_extracted_data_get_text(data);
        if (txt && *txt) {
          *out_utf8_text = txt;
        }
        doctotext_free_extracted_data(data);
      }
      if (exc) {
        doctotext_free_exception(exc);
      }
      doctotext_free_extractor_params(params);

      if (!tmp_log_path.empty() && std::filesystem::exists(tmp_log_path)) {
        std::ifstream tmp_ifs(tmp_log_path);
        if (tmp_ifs.is_open()) {
          std::string dt_logs((std::istreambuf_iterator<char>(tmp_ifs)),
                              std::istreambuf_iterator<char>());
          tmp_ifs.close();
          std::lock_guard<std::mutex> lock(g_dt_mutex);
          if (g_dt_file_stream && g_dt_file_stream->is_open()) {
            *g_dt_file_stream << dt_logs;
          }
        }
        std::error_code ec;
        std::filesystem::remove(tmp_log_path, ec);
      }
    }

    if (enable_logging) {
      std::lock_guard<std::mutex> lock(g_dt_mutex);
      if (g_dt_file_stream && g_dt_file_stream->is_open()) {
        g_dt_file_stream->flush();
      }
    }
  } catch (...) {
    // Graceful fallback to secondary extractors
  }
#endif

  // 2. Secondary / Fallback: If DocToText extraction was empty or failed, use internal extractors
  if (out_utf8_text->empty()) {
    if (lower == "docx") {
      ParseDocx(file_path, out_utf8_text);
    } else if (lower == "xlsx") {
      ParseXlsx(file_path, out_utf8_text);
    } else if (lower == "pptx") {
      ParsePptx(file_path, out_utf8_text);
    } else if (lower == "pdf") {
      ParsePdf(file_path, out_utf8_text, is_scanned_pdf);
    } else if (lower == "doc" || lower == "xls" || lower == "ppt" ||
               lower == "wps" || lower == "et" || lower == "dps" || lower == "rtf") {
      ParseLegacyOffice(file_path, lower, out_utf8_text);
    }
  }

  // Check if PDF is a scanned image
  if (lower == "pdf") {
    size_t non_whitespace = 0;
    for (char c : *out_utf8_text) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        non_whitespace++;
      }
    }
    uint64_t file_size = GetFileSize(file_path);
    if (non_whitespace < 15 && file_size > 10 * 1024) {
      if (is_scanned_pdf) *is_scanned_pdf = true;
    }
  }

  // 3. Sanitize UTF-8 output
  if (!out_utf8_text->empty()) {
    std::string sanitized;
    sanitized.reserve(out_utf8_text->size());
    utf8::replace_invalid(out_utf8_text->begin(), out_utf8_text->end(),
                          std::back_inserter(sanitized), ' ');
    *out_utf8_text = std::move(sanitized);
    return true;
  }

  // If this is a valid scanned PDF, treat as successful extraction with empty text
  if (lower == "pdf" && is_scanned_pdf && *is_scanned_pdf) {
    return true;
  }

  return false;
}

namespace {

class ZipFileScope {
 public:
  ZipFileScope() : file_(nullptr) {
    std::memset(&zip_, 0, sizeof(zip_));
  }
  ~ZipFileScope() {
    Close();
  }

  bool Open(const std::string& file_path) {
    Close();
    file_ = common::EncodingUtils::FopenUtf8(file_path, "rb");
    if (!file_) return false;
    if (mz_zip_reader_init_cfile(&zip_, file_, 0, 0) != MZ_TRUE) {
      std::fclose(file_);
      file_ = nullptr;
      return false;
    }
    return true;
  }

  void Close() {
    if (file_) {
      mz_zip_reader_end(&zip_);
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  mz_zip_archive* zip() { return &zip_; }

 private:
  mz_zip_archive zip_;
  FILE* file_;
};

}  // namespace

bool L2OfficeParser::ParseDocx(const std::string& file_path,
                              std::string* out_utf8) {
  ZipFileScope zip_scope;
  if (!zip_scope.Open(file_path)) {
    return false;
  }
  mz_zip_archive* zip = zip_scope.zip();

  size_t uncomp_size = 0;
  void* data = mz_zip_reader_extract_file_to_heap(zip, "word/document.xml",
                                                  &uncomp_size, 0);
  if (!data) {
    return false;
  }

  std::string xml(static_cast<const char*>(data), uncomp_size);
  mz_free(data);

  *out_utf8 = ExtractXmlText(xml, true);
  return !out_utf8->empty();
}

bool L2OfficeParser::ParseXlsx(const std::string& file_path,
                              std::string* out_utf8) {
  ZipFileScope zip_scope;
  if (!zip_scope.Open(file_path)) {
    return false;
  }
  mz_zip_archive* zip = zip_scope.zip();

  std::ostringstream oss;

  // 1. Extract sharedStrings.xml (primary string table in XLSX)
  size_t shared_size = 0;
  void* shared_data = mz_zip_reader_extract_file_to_heap(
      zip, "xl/sharedStrings.xml", &shared_size, 0);
  if (shared_data) {
    std::string shared_xml(static_cast<const char*>(shared_data), shared_size);
    mz_free(shared_data);
    oss << ExtractXmlText(shared_xml, true) << "\n";
  }

  // 2. Extract worksheet cell contents
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(zip); ++i) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(zip, i, &file_stat)) continue;

    std::string filename(file_stat.m_filename);
    if (filename.rfind("xl/worksheets/sheet", 0) == 0 &&
        filename.size() >= 4 &&
        filename.substr(filename.size() - 4) == ".xml") {
      size_t sheet_size = 0;
      void* sheet_data = mz_zip_reader_extract_file_to_heap(
          zip, file_stat.m_filename, &sheet_size, 0);
      if (sheet_data) {
        std::string sheet_xml(static_cast<const char*>(sheet_data), sheet_size);
        mz_free(sheet_data);
        oss << ExtractXmlText(sheet_xml, false) << "\n";
      }
    }
  }

  *out_utf8 = oss.str();
  return !out_utf8->empty();
}

bool L2OfficeParser::ParsePptx(const std::string& file_path,
                              std::string* out_utf8) {
  ZipFileScope zip_scope;
  if (!zip_scope.Open(file_path)) {
    return false;
  }
  mz_zip_archive* zip = zip_scope.zip();

  std::ostringstream oss;

  // Extract all slide*.xml
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(zip); ++i) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(zip, i, &file_stat)) continue;

    std::string filename(file_stat.m_filename);
    if (filename.rfind("ppt/slides/slide", 0) == 0 &&
        filename.size() >= 4 &&
        filename.substr(filename.size() - 4) == ".xml") {
      size_t slide_size = 0;
      void* slide_data = mz_zip_reader_extract_file_to_heap(
          zip, file_stat.m_filename, &slide_size, 0);
      if (slide_data) {
        std::string slide_xml(static_cast<const char*>(slide_data), slide_size);
        mz_free(slide_data);
        oss << ExtractXmlText(slide_xml, true) << "\n";
      }
    }
  }

  *out_utf8 = oss.str();
  return !out_utf8->empty();
}

bool L2OfficeParser::ParsePdf(const std::string& file_path,
                             std::string* out_utf8, bool* is_scanned) {
  if (is_scanned) *is_scanned = false;
  if (!out_utf8) return false;
  out_utf8->clear();

  // Pure C++ fallback for embedded text strings
  ParseLegacyOffice(file_path, "pdf", out_utf8);

  uint64_t file_size = GetFileSize(file_path);
  if (out_utf8->size() < 15 && file_size > 10 * 1024) {
    if (is_scanned) *is_scanned = true;
  }
  return !out_utf8->empty() || (is_scanned && *is_scanned);
}

bool L2OfficeParser::ParseLegacyOffice(const std::string& file_path,
                                      const std::string& ext,
                                      std::string* out_utf8) {
  // Extract both UTF-16LE and ANSI / UTF-8 characters directly from disk via stream
  auto ifs = common::EncodingUtils::OpenIfstream(file_path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }

  std::string extracted;
  extracted.reserve(8192);

  auto is_valid_char = [](wchar_t wc) {
    if (wc >= 0x0020 && wc <= 0x007E) return true;  // Printable ASCII
    if (wc == L'\n' || wc == L'\r' || wc == L'\t') return true;
    if (wc >= 0x00A0 && wc < 0xD800) return true;   // Unicode (including CJK 0x4E00 - 0x9FFF)
    if (wc >= 0xE000 && wc <= 0xFFFD) return true;
    return false;
  };

  std::wstring current_wstr;
  std::string current_ansi;
  char buf[65536];
  uint8_t pending_byte = 0;
  bool has_pending = false;

  while (ifs) {
    ifs.read(buf, sizeof(buf));
    std::streamsize bytes_read = ifs.gcount();
    if (bytes_read <= 0) break;

    for (std::streamsize idx = 0; idx < bytes_read; ++idx) {
      uint8_t byte = static_cast<uint8_t>(buf[idx]);

      // 1. ANSI scanner
      if (byte >= 32 || byte == '\n' || byte == '\t' || byte >= 0x80) {
        current_ansi.push_back(static_cast<char>(byte));
      } else {
        if (current_ansi.size() >= 4) {
          extracted += common::EncodingUtils::ToUtf8(current_ansi);
          extracted.push_back(' ');
        }
        current_ansi.clear();
      }

      // 2. UTF-16LE scanner
      if (!has_pending) {
        pending_byte = byte;
        has_pending = true;
      } else {
        wchar_t wc = static_cast<wchar_t>(pending_byte | (byte << 8));
        has_pending = false;
        if (is_valid_char(wc)) {
          current_wstr.push_back(wc);
        } else {
          if (current_wstr.size() >= 3) {
            extracted += common::EncodingUtils::WideToUtf8(current_wstr);
            extracted.push_back(' ');
          }
          current_wstr.clear();
        }
      }
    }
  }

  if (current_ansi.size() >= 4) {
    extracted += common::EncodingUtils::ToUtf8(current_ansi);
    extracted.push_back(' ');
  }
  if (current_wstr.size() >= 3) {
    extracted += common::EncodingUtils::WideToUtf8(current_wstr);
    extracted.push_back(' ');
  }

  *out_utf8 = std::move(extracted);
  return !out_utf8->empty();
}

std::string L2OfficeParser::ExtractXmlText(const std::string& xml_content,
                                          bool insert_newlines) {
  std::string result;
  result.reserve(xml_content.size() / 3);

  bool in_tag = false;
  std::string tag_name;

  for (size_t i = 0; i < xml_content.size(); ++i) {
    char c = xml_content[i];
    if (c == '<') {
      in_tag = true;
      tag_name.clear();
    } else if (c == '>') {
      in_tag = false;
      // Emit line break on paragraph or table row boundary
      if (insert_newlines) {
        if (tag_name == "/w:p" || tag_name == "w:p" ||
            tag_name == "/a:p" || tag_name == "a:p" ||
            tag_name == "w:br" || tag_name == "a:br" ||
            tag_name == "/row" || tag_name == "/tr") {
          result.push_back('\n');
        } else if (tag_name == "w:tab") {
          result.push_back('\t');
        }
      }
    } else {
      if (in_tag) {
        if (!std::isspace(static_cast<unsigned char>(c)) && tag_name.size() < 16) {
          tag_name.push_back(c);
        }
      } else {
        result.push_back(c);
      }
    }
  }

  return UnescapeXmlEntities(result);
}

std::string L2OfficeParser::UnescapeXmlEntities(const std::string& text) {
  std::string result;
  result.reserve(text.size());

  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '&') {
      if (text.compare(i, 4, "&lt;") == 0) {
        result.push_back('<');
        i += 3;
      } else if (text.compare(i, 4, "&gt;") == 0) {
        result.push_back('>');
        i += 3;
      } else if (text.compare(i, 5, "&amp;") == 0) {
        result.push_back('&');
        i += 4;
      } else if (text.compare(i, 6, "&quot;") == 0) {
        result.push_back('"');
        i += 5;
      } else if (text.compare(i, 6, "&apos;") == 0) {
        result.push_back('\'');
        i += 5;
      } else {
        result.push_back(text[i]);
      }
    } else {
      result.push_back(text[i]);
    }
  }

  return result;
}

}  // namespace worker
}  // namespace soeasy
