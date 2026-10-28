// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/l2_office_parser.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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

#include "miniz.h"
#include "plain_text_extractor.h"
#include "utf8.h"

namespace soeasy {
namespace worker {

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

std::string RunCommandCaptureStdout(const std::string& cmd) {
  std::array<char, 4096> buffer;
  std::string result;
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    return "";
  }
  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }
  pclose(pipe);
  return result;
}

uint64_t GetFileSize(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    return static_cast<uint64_t>(st.st_size);
  }
  return 0;
}

}  // namespace

L2OfficeParser::L2OfficeParser() {
  supported_exts_ = {
      "docx", "xlsx", "pptx", "pdf", "doc", "xls", "ppt", "rtf",
      "odt", "ods", "odp", "xlsb", "eml", "pages", "numbers", "key"
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

bool L2OfficeParser::Parse(const std::string& file_path, const std::string& ext,
                           std::string* out_utf8_text, bool* is_scanned_pdf) {
  if (!out_utf8_text) return false;
  out_utf8_text->clear();
  if (is_scanned_pdf) *is_scanned_pdf = false;

  std::string lower = ext;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

  // Fast-path for formats natively and robustly handled by our internal OOXML extractors
  if (lower == "xlsx") {
    return ParseXlsx(file_path, out_utf8_text);
  }
  if (lower == "pptx") {
    return ParsePptx(file_path, out_utf8_text);
  }

  // 1. Primary: Use DocToText (lithiumFlower/doctotext) for comprehensive multi-format extraction
  try {
    doctotext::PlainTextExtractor extractor;
    bool enable_logging = false;
    std::ostream* target_stream = &g_null_stream;
    {
      std::lock_guard<std::mutex> lock(g_dt_mutex);
      if (g_dt_logging_enabled && g_dt_file_stream && g_dt_file_stream->is_open()) {
        enable_logging = true;
        target_stream = g_dt_file_stream.get();
        *target_stream << "\n[DocToText] Parsing: " << file_path << " (ext: " << lower << ")\n"
                       << std::flush;
      }
    }
    extractor.setVerboseLogging(enable_logging);
    extractor.setLogStream(*target_stream);
    doctotext::PlainTextExtractor::ParserType ptype =
        extractor.parserTypeByFileExtension(lower);
    if (ptype == doctotext::PlainTextExtractor::PARSER_AUTO) {
      if (lower == "docx" || lower == "odt" || lower == "ods" || lower == "odp") {
        ptype = doctotext::PlainTextExtractor::PARSER_ODF_OOXML;
      } else if (lower == "doc") {
        ptype = doctotext::PlainTextExtractor::PARSER_DOC;
      } else if (lower == "xls") {
        ptype = doctotext::PlainTextExtractor::PARSER_XLS;
      } else if (lower == "ppt") {
        ptype = doctotext::PlainTextExtractor::PARSER_PPT;
      } else if (lower == "rtf") {
        ptype = doctotext::PlainTextExtractor::PARSER_RTF;
      } else if (lower == "pdf") {
        ptype = doctotext::PlainTextExtractor::PARSER_PDF;
      } else if (lower == "xlsb") {
        ptype = doctotext::PlainTextExtractor::PARSER_XLSB;
      } else if (lower == "eml") {
        ptype = doctotext::PlainTextExtractor::PARSER_EML;
      } else if (lower == "pages" || lower == "numbers" || lower == "key") {
        ptype = doctotext::PlainTextExtractor::PARSER_IWORK;
      }
    }

    if (ptype != doctotext::PlainTextExtractor::PARSER_AUTO) {
      std::string doctotext_out;
      if (extractor.processFile(ptype, false, file_path, doctotext_out) &&
          !doctotext_out.empty()) {
        *out_utf8_text = std::move(doctotext_out);
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
    } else if (lower == "doc" || lower == "xls" || lower == "ppt") {
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

  return false;
}

bool L2OfficeParser::ParseDocx(const std::string& file_path,
                              std::string* out_utf8) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, file_path.c_str(), 0)) {
    return false;
  }

  size_t uncomp_size = 0;
  void* data = mz_zip_reader_extract_file_to_heap(&zip, "word/document.xml",
                                                  &uncomp_size, 0);
  if (!data) {
    mz_zip_reader_end(&zip);
    return false;
  }

  std::string xml(static_cast<const char*>(data), uncomp_size);
  mz_free(data);
  mz_zip_reader_end(&zip);

  *out_utf8 = ExtractXmlText(xml, true);
  return !out_utf8->empty();
}

bool L2OfficeParser::ParseXlsx(const std::string& file_path,
                              std::string* out_utf8) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, file_path.c_str(), 0)) {
    return false;
  }

  std::ostringstream oss;

  // 1. Extract sharedStrings.xml (primary string table in XLSX)
  size_t shared_size = 0;
  void* shared_data = mz_zip_reader_extract_file_to_heap(
      &zip, "xl/sharedStrings.xml", &shared_size, 0);
  if (shared_data) {
    std::string shared_xml(static_cast<const char*>(shared_data), shared_size);
    mz_free(shared_data);
    oss << ExtractXmlText(shared_xml, true) << "\n";
  }

  // 2. Extract worksheet cell contents
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) continue;

    std::string filename(file_stat.m_filename);
    if (filename.rfind("xl/worksheets/sheet", 0) == 0 &&
        filename.size() >= 4 &&
        filename.substr(filename.size() - 4) == ".xml") {
      size_t sheet_size = 0;
      void* sheet_data = mz_zip_reader_extract_file_to_heap(
          &zip, file_stat.m_filename, &sheet_size, 0);
      if (sheet_data) {
        std::string sheet_xml(static_cast<const char*>(sheet_data), sheet_size);
        mz_free(sheet_data);
        oss << ExtractXmlText(sheet_xml, false) << "\n";
      }
    }
  }

  mz_zip_reader_end(&zip);
  *out_utf8 = oss.str();
  return !out_utf8->empty();
}

bool L2OfficeParser::ParsePptx(const std::string& file_path,
                              std::string* out_utf8) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  if (!mz_zip_reader_init_file(&zip, file_path.c_str(), 0)) {
    return false;
  }

  std::ostringstream oss;

  // Extract all slide*.xml
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) continue;

    std::string filename(file_stat.m_filename);
    if (filename.rfind("ppt/slides/slide", 0) == 0 &&
        filename.size() >= 4 &&
        filename.substr(filename.size() - 4) == ".xml") {
      size_t slide_size = 0;
      void* slide_data = mz_zip_reader_extract_file_to_heap(
          &zip, file_stat.m_filename, &slide_size, 0);
      if (slide_data) {
        std::string slide_xml(static_cast<const char*>(slide_data), slide_size);
        mz_free(slide_data);
        oss << ExtractXmlText(slide_xml, true) << "\n";
      }
    }
  }

  mz_zip_reader_end(&zip);
  *out_utf8 = oss.str();
  return !out_utf8->empty();
}

bool L2OfficeParser::ParsePdf(const std::string& file_path,
                             std::string* out_utf8, bool* is_scanned) {
  if (is_scanned) *is_scanned = false;

  // Try extracting text via pdftotext (Poppler)
  std::string cmd = "pdftotext -enc UTF-8 -layout \"" + file_path + "\" - 2>/dev/null";
  std::string output = RunCommandCaptureStdout(cmd);

  *out_utf8 = std::move(output);
  return !out_utf8->empty();
}

bool L2OfficeParser::ParseLegacyOffice(const std::string& file_path,
                                      const std::string& ext,
                                      std::string* out_utf8) {
  std::string cmd;
  if (ext == "doc") {
    cmd = "catdoc -d utf-8 \"" + file_path + "\" 2>/dev/null";
  } else if (ext == "xls") {
    cmd = "xls2csv -d utf-8 \"" + file_path + "\" 2>/dev/null";
  } else {
    cmd = "catdoc -d utf-8 \"" + file_path + "\" 2>/dev/null";
  }

  std::string output = RunCommandCaptureStdout(cmd);
  if (!output.empty()) {
    *out_utf8 = std::move(output);
    return true;
  }

  // Fallback: extract contiguous printable ASCII / UTF-8 characters from binary stream
  std::ifstream ifs(file_path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }

  std::string content((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  std::string extracted;
  std::string chunk;
  for (unsigned char c : content) {
    if (c >= 32 || c == '\n' || c == '\t' || c >= 0x80) {
      chunk += static_cast<char>(c);
    } else {
      if (chunk.size() >= 4) {
        extracted += chunk;
        extracted += " ";
      }
      chunk.clear();
    }
  }
  if (chunk.size() >= 4) {
    extracted += chunk;
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
