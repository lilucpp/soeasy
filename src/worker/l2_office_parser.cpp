// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "worker/l2_office_parser.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#include <process.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/encoding_utils.h"
#include "common/logger.h"
#include "common/system_env.h"
#include "doctotext_c_api.h"
#if !defined(DOCTOTEXT_VERSION_MAJOR) || !defined(DOCTOTEXT_VERSION_MINOR) || !defined(DOCTOTEXT_VERSION_PATCH) || \
    DOCTOTEXT_VERSION_MAJOR != 5 || DOCTOTEXT_VERSION_MINOR != 1 || DOCTOTEXT_VERSION_PATCH != 5
#error "DocToText header version mismatch: SoEasy requires DocToText 5.1.5 headers."
#endif
#include "miniz.h"
#include "utf8.h"
#include "worker/ocr/image_filter.h"
#include "worker/ocr/ocr_engine_factory.h"

namespace soeasy {
namespace worker {

namespace {

#ifdef _WIN32
constexpr const char *kNullDevice = "NUL";
inline unsigned long CurrentProcessId() { return static_cast<unsigned long>(_getpid()); }
#else
constexpr const char *kNullDevice = "/dev/null";
inline unsigned long CurrentProcessId() { return static_cast<unsigned long>(getpid()); }
#endif

static std::mutex g_dt_mutex;
static bool g_dt_logging_enabled = false;
static std::string g_dt_log_path = "";
static std::unique_ptr<std::ofstream> g_dt_file_stream;
static std::atomic<bool> s_embedded_image_ocr{false};
static std::atomic<uint64_t> g_dt_tmp_seq{0};

/// Convert an ASCII extension string to lowercase safely without unsigned char UB.
std::string ToLowerAscii(const std::string &s) {
  std::string lower = s;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

/// Map lowercase file extension to explicit DocToText parser type.
int ResolveDocToTextParserType(const std::string &lower_ext) {
  static const std::unordered_map<std::string, int> kParserTypeMap = {
      {"docx", DOCTOTEXT_PARSER_ODF_OOXML}, {"xlsx", DOCTOTEXT_PARSER_ODF_OOXML},
      {"pptx", DOCTOTEXT_PARSER_ODF_OOXML}, {"odt", DOCTOTEXT_PARSER_ODF_OOXML},
      {"ods", DOCTOTEXT_PARSER_ODF_OOXML},  {"odp", DOCTOTEXT_PARSER_ODF_OOXML},
      {"odg", DOCTOTEXT_PARSER_ODF_OOXML},  {"doc", DOCTOTEXT_PARSER_DOC},
      {"wps", DOCTOTEXT_PARSER_DOC},        {"xls", DOCTOTEXT_PARSER_XLS},
      {"et", DOCTOTEXT_PARSER_XLS},         {"ppt", DOCTOTEXT_PARSER_PPT},
      {"dps", DOCTOTEXT_PARSER_PPT},        {"rtf", DOCTOTEXT_PARSER_RTF},
      {"pdf", DOCTOTEXT_PARSER_PDF},        {"xlsb", DOCTOTEXT_PARSER_XLSB},
      {"eml", DOCTOTEXT_PARSER_EML},        {"fodt", DOCTOTEXT_PARSER_ODFXML},
      {"fods", DOCTOTEXT_PARSER_ODFXML},    {"fodp", DOCTOTEXT_PARSER_ODFXML},
      {"pages", DOCTOTEXT_PARSER_IWORK},    {"numbers", DOCTOTEXT_PARSER_IWORK},
      {"key", DOCTOTEXT_PARSER_IWORK},      {"keynote", DOCTOTEXT_PARSER_IWORK},
  };
  auto it = kParserTypeMap.find(lower_ext);
  return it != kParserTypeMap.end() ? it->second : DOCTOTEXT_PARSER_AUTO;
}

bool HasMeaningfulText(const std::string &text) {
  return std::any_of(text.begin(), text.end(),
                     [](unsigned char c) { return !std::isspace(c); });
}

/// Count non-whitespace Unicode codepoints in sanitized valid UTF-8 text.
size_t CountNonWhitespaceCodepoints(const std::string &sanitized_utf8) {
  size_t count = 0;
  auto it = sanitized_utf8.begin();
  const auto end = sanitized_utf8.end();
  while (it != end) {
    uint32_t cp = utf8::next(it, end);
    if (cp > 0x7Fu || !std::isspace(static_cast<unsigned char>(cp))) {
      ++count;
    }
  }
  return count;
}

uint64_t GetFileSize(const std::string &path) {
  uint64_t size = 0;
  if (common::EncodingUtils::GetFileStat(path, &size, nullptr)) {
    return size;
  }
  return 0;
}

/// RAII guard for miniz zip archive handles opened via UTF-8 safe FILE*.
class ZipFileScope {
 public:
  ZipFileScope() : file_(nullptr) { std::memset(&zip_, 0, sizeof(zip_)); }
  ~ZipFileScope() { Close(); }

  ZipFileScope(const ZipFileScope &) = delete;
  ZipFileScope &operator=(const ZipFileScope &) = delete;

  bool Open(const std::string &file_path) {
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

  mz_zip_archive *zip() { return &zip_; }

 private:
  mz_zip_archive zip_;
  FILE *file_;
};

constexpr uint64_t kMaxFallbackZipEntryBytes = 64ULL * 1024ULL * 1024ULL;

bool AsciiEqualsIgnoreCase(const std::string &a, const char *b) {
  const size_t b_len = std::strlen(b);
  if (a.size() != b_len) {
    return false;
  }
  for (size_t i = 0; i < b_len; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string NormalizeZipRelPath(const std::string &raw_path) {
  std::vector<std::string> parts;
  size_t i = 0;
  while (i < raw_path.size()) {
    while (i < raw_path.size() && (raw_path[i] == '/' || raw_path[i] == '\\')) {
      ++i;
    }
    if (i >= raw_path.size()) {
      break;
    }
    size_t j = i;
    while (j < raw_path.size() && raw_path[j] != '/' && raw_path[j] != '\\') {
      ++j;
    }
    std::string seg = raw_path.substr(i, j - i);
    i = j;
    if (seg == ".") {
      continue;
    }
    if (seg == "..") {
      if (!parts.empty()) {
        parts.pop_back();
      }
      continue;
    }
    parts.push_back(std::move(seg));
  }
  std::string out;
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k > 0) {
      out.push_back('/');
    }
    out += parts[k];
  }
  return out;
}

std::string ReadZipEntryString(mz_zip_archive *zip, const char *entry_name) {
  const int file_idx = mz_zip_reader_locate_file(zip, entry_name, nullptr, 0);
  if (file_idx < 0) {
    return "";
  }
  mz_zip_archive_file_stat stat{};
  if (!mz_zip_reader_file_stat(zip, static_cast<mz_uint>(file_idx), &stat) ||
      stat.m_uncomp_size > kMaxFallbackZipEntryBytes) {
    return "";
  }
  size_t uncomp_size = 0;
  void *data = mz_zip_reader_extract_to_heap(zip, static_cast<mz_uint>(file_idx), &uncomp_size, 0);
  if (!data) {
    return "";
  }
  std::unique_ptr<char, decltype(&mz_free)> data_guard(static_cast<char *>(data), mz_free);
  return std::string(data_guard.get(), uncomp_size);
}

/// Helper to safely extract and parse an XML entry from a zip archive using RAII.
std::string ExtractZipEntryXmlText(mz_zip_archive *zip, const char *entry_name, bool strip_tags) {
  const std::string xml = ReadZipEntryString(zip, entry_name);
  if (xml.empty()) {
    return "";
  }
  return L2OfficeParser::ExtractXmlText(xml, strip_tags);
}

std::string ResolveAlternateDocxPath(mz_zip_archive *zip) {
  const std::string rels = ReadZipEntryString(zip, "_rels/.rels");
  if (rels.empty()) {
    return "";
  }
  size_t pos = 0;
  while (pos < rels.size()) {
    const size_t open = rels.find('<', pos);
    if (open == std::string::npos) {
      break;
    }
    if (rels.compare(open, 4, "<!--") == 0) {
      const size_t cmt_end = rels.find("-->", open + 4);
      if (cmt_end == std::string::npos) {
        break;
      }
      pos = cmt_end + 3;
      continue;
    }
    if (rels.compare(open, 2, "<?") == 0) {
      const size_t pi_end = rels.find("?>", open + 2);
      if (pi_end == std::string::npos) {
        break;
      }
      pos = pi_end + 2;
      continue;
    }
    if (open + 1 < rels.size() && (rels[open + 1] == '!' || rels[open + 1] == '/')) {
      const size_t skip_end = rels.find('>', open + 2);
      if (skip_end == std::string::npos) {
        break;
      }
      pos = skip_end + 1;
      continue;
    }

    // Find matching unquoted '>' for start/empty-element tag
    char quote = '\0';
    size_t close = std::string::npos;
    for (size_t i = open + 1; i < rels.size(); ++i) {
      const char ch = rels[i];
      if (quote != '\0') {
        if (ch == quote) {
          quote = '\0';
        }
      } else if (ch == '"' || ch == '\'') {
        quote = ch;
      } else if (ch == '>') {
        close = i;
        break;
      }
    }
    if (close == std::string::npos) {
      break;
    }
    pos = close + 1;

    // Parse tag local name (supporting optional namespace prefix such as ns:Relationship)
    size_t p = open + 1;
    while (p < close && std::isspace(static_cast<unsigned char>(rels[p]))) {
      ++p;
    }
    const size_t name_start = p;
    while (p < close && !std::isspace(static_cast<unsigned char>(rels[p])) && rels[p] != '/') {
      ++p;
    }
    const size_t colon = rels.rfind(':', p - 1);
    const size_t local_start = (colon != std::string::npos && colon >= name_start) ? (colon + 1) : name_start;
    if (p <= local_start || rels.compare(local_start, p - local_start, "Relationship") != 0) {
      continue;
    }

    // Parse attributes cleanly within [p, close) respecting quotes
    std::string type;
    std::string raw_target;
    std::string target_mode;
    while (p < close) {
      while (p < close && (std::isspace(static_cast<unsigned char>(rels[p])) || rels[p] == '/')) {
        ++p;
      }
      if (p >= close) {
        break;
      }
      const size_t attr_start = p;
      while (p < close && !std::isspace(static_cast<unsigned char>(rels[p])) && rels[p] != '=' && rels[p] != '/') {
        ++p;
      }
      const std::string attr_name = rels.substr(attr_start, p - attr_start);
      while (p < close && std::isspace(static_cast<unsigned char>(rels[p]))) {
        ++p;
      }
      if (p >= close || rels[p] != '=') {
        continue;
      }
      ++p;
      while (p < close && std::isspace(static_cast<unsigned char>(rels[p]))) {
        ++p;
      }
      if (p >= close || (rels[p] != '"' && rels[p] != '\'')) {
        continue;
      }
      const char q = rels[p++];
      const size_t val_start = p;
      while (p < close && rels[p] != q) {
        ++p;
      }
      if (p >= close) {
        break;
      }
      const std::string attr_val = rels.substr(val_start, p - val_start);
      ++p;

      if (attr_name == "Type") {
        type = L2OfficeParser::UnescapeXmlEntities(attr_val);
      } else if (attr_name == "Target") {
        raw_target = L2OfficeParser::UnescapeXmlEntities(attr_val);
      } else if (attr_name == "TargetMode") {
        target_mode = L2OfficeParser::UnescapeXmlEntities(attr_val);
      }
    }

    if (AsciiEqualsIgnoreCase(target_mode, "External")) {
      continue;
    }
    constexpr const char kSuffix[] = "/officeDocument";
    constexpr size_t kSuffixLen = sizeof(kSuffix) - 1;
    if (type.size() < kSuffixLen || type.compare(type.size() - kSuffixLen, kSuffixLen, kSuffix) != 0) {
      continue;
    }
    const std::string target = NormalizeZipRelPath(raw_target);
    if (target.size() >= 4 && target[target.size() - 4] == '.' &&
        std::tolower(static_cast<unsigned char>(target[target.size() - 3])) == 'x' &&
        std::tolower(static_cast<unsigned char>(target[target.size() - 2])) == 'm' &&
        std::tolower(static_cast<unsigned char>(target[target.size() - 1])) == 'l' &&
        mz_zip_reader_locate_file(zip, target.c_str(), nullptr, 0) >= 0) {
      return target;
    }
  }
  return "";
}

/// Safely collect error messages from a DocToTextException using bounded iteration.
std::string ExtractDocToTextErrors(DocToTextException *exc) {
  if (!exc) {
    return {};
  }
  const size_t count = doctotext_exception_error_messages_count(exc);
  const size_t limit = std::min<size_t>(count, 4);
  std::string combined;
  for (size_t i = 0; i < limit; ++i) {
    const char *msg = doctotext_exception_get_error_message(exc, i);
    if (!msg || !*msg) {
      continue;
    }
    if (!combined.empty()) {
      combined += "; ";
    }
    combined += msg;
  }
  return combined;
}

/// Remove a temporary DocToText log file automatically on scope exit.
struct TempFileCleaner {
  std::string path;
  ~TempFileCleaner() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }
};

/// Dispatch secondary structured extractors only when DocToText failed (never scrape raw legacy binary bytes).
bool ApplyFallbackExtractor(const std::string &file_path, const std::string &lower_ext, std::string *out_utf8_text,
                            bool *is_scanned_pdf) {
  out_utf8_text->clear();
  bool ok = false;
  if (lower_ext == "docx") {
    ok = L2OfficeParser::ParseDocx(file_path, out_utf8_text);
  } else if (lower_ext == "xlsx") {
    ok = L2OfficeParser::ParseXlsx(file_path, out_utf8_text);
  } else if (lower_ext == "pptx") {
    ok = L2OfficeParser::ParsePptx(file_path, out_utf8_text);
  } else if (lower_ext == "pdf") {
    ok = L2OfficeParser::ParsePdf(file_path, out_utf8_text, is_scanned_pdf);
  }
  if (!ok) {
    out_utf8_text->clear();
  }
  return ok;
}

}  // namespace

L2OfficeParser::L2OfficeParser()
    : supported_exts_{"docx", "xlsx",  "pptx",    "pdf", "doc",     "xls",  "ppt",  "rtf",
                      "odt",  "ods",   "odp",     "odg", "fodt",    "fods", "fodp", "xlsb",
                      "eml",  "pages", "numbers", "key", "keynote", "wps",  "et",   "dps"},
      last_embedded_ocr_active_time_(std::chrono::steady_clock::now()) {}

L2OfficeParser::~L2OfficeParser() = default;

struct L2OfficeParser::EmbeddedOcrBudget {
  L2OfficeParser *parser = nullptr;
  std::chrono::steady_clock::time_point deadline;
  uint32_t budget_ms = 0;
  size_t processed_count = 0;
  size_t skipped_count = 0;
  bool hit_limit = false;

  bool IsExpired() {
    if (hit_limit) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      hit_limit = true;
      return true;
    }
    return false;
  }
};

std::string L2OfficeParser::RunEmbeddedImageOcr(const std::string &image_name, const char *image_buffer,
                                                size_t image_size, EmbeddedOcrBudget *budget) {
  if (!image_buffer || image_size < 128 || image_size > 30ULL * 1024ULL * 1024ULL) return "";
  if (budget && budget->IsExpired()) {
    ++budget->skipped_count;
    return "";
  }

  const ocr::ImageFilter filter;
  // 1. Fast zero-copy header probe before decoding (< 1 microsecond, 0 memory allocation)
  auto dim = ocr::ImageFilter::ProbeBufferDimensions(reinterpret_cast<const uint8_t *>(image_buffer), image_size);
  if (dim.valid && filter.CheckDimensions(dim.width, dim.height) != ocr::FilterDecision::kPass) {
    return "";  // Drop icons, bullets, extreme strips, and pixel bombs without decoding
  }

  try {
    std::vector<uint8_t> buf(reinterpret_cast<const uint8_t *>(image_buffer),
                             reinterpret_cast<const uint8_t *>(image_buffer) + image_size);
    cv::Mat raw_img = cv::imdecode(buf, cv::IMREAD_UNCHANGED);
    cv::Mat img = ocr::ImageFilter::NormalizeToBgr(raw_img);
    if (img.empty()) return "";

    if (filter.CheckDecodedImage(img) != ocr::FilterDecision::kPass) {
      return "";
    }

    std::lock_guard<std::mutex> lock(ocr_mutex_);
    if (!ocr_engine_) {
      // Use max limit_side_len = 640 for embedded images (optimal balance for speed, memory and accuracy)
      ocr_engine_ = ocr::OcrEngineFactory::CreateDefaultEngine("", ocr::OcrMode::kNormal, 640);
    }
    if (budget && budget->IsExpired()) {
      ++budget->skipped_count;
      return "";
    }
    if (!ocr_engine_) return "";
    if (budget) {
      ++budget->processed_count;
    }
    last_embedded_ocr_active_time_ = std::chrono::steady_clock::now();

    auto ocr_res = ocr_engine_->Recognize(img);
    last_embedded_ocr_active_time_ = std::chrono::steady_clock::now();
    if (ocr_res.success &&
        ocr::ImageFilter::CountEffectiveChars(ocr_res.full_text) >= filter.options().min_effective_chars) {
      return ocr_res.full_text;
    }
  } catch (const std::exception &e) {
    LOG_WARN("[OfficeParser] Embedded image OCR failed for {}: {}", image_name, e.what());
  } catch (...) {
    LOG_WARN("[OfficeParser] Embedded image OCR failed for {} with unknown exception", image_name);
  }
  return "";
}

const char *L2OfficeParser::DocToTextImageParserCallbackC(const char *image_name, const char *image_buffer,
                                                         size_t image_size, void *user_data) {
  if (!user_data) return nullptr;
  auto *budget = static_cast<EmbeddedOcrBudget *>(user_data);
  if (!budget->parser) return nullptr;
  if (budget->IsExpired()) {
    ++budget->skipped_count;
    return nullptr;
  }
  static thread_local std::string tls_ocr_result;
  tls_ocr_result =
      budget->parser->RunEmbeddedImageOcr(image_name ? image_name : "", image_buffer, image_size, budget);
  return tls_ocr_result.empty() ? nullptr : tls_ocr_result.c_str();
}

void L2OfficeParser::ExtractWithDocToText(const std::string &file_path, const std::string &lower_ext,
                                          uint32_t timeout_ms, std::string *out_utf8_text, int *out_dt_status,
                                          std::string *out_dt_error, EmbeddedOcrBudget *out_budget) {
  if (out_dt_status) *out_dt_status = DOCTOTEXT_STATUS_OK;
  if (out_dt_error) out_dt_error->clear();
  const uint32_t effective_timeout_ms = timeout_ms > 0 ? timeout_ms : 60000;
  const uint32_t ocr_budget_ms =
      static_cast<uint32_t>(std::max<uint64_t>(1, (static_cast<uint64_t>(effective_timeout_ms) * 3) / 4));
  EmbeddedOcrBudget local_budget;
  local_budget.parser = this;
  local_budget.budget_ms = ocr_budget_ms;
  local_budget.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ocr_budget_ms);

  TempFileCleaner temp_cleaner;
  bool enable_logging = false;
  try {
    std::string log_file_path;
    {
      std::lock_guard<std::mutex> lock(g_dt_mutex);
      if (g_dt_logging_enabled && g_dt_file_stream && g_dt_file_stream->is_open()) {
        enable_logging = true;
        log_file_path = g_dt_log_path;
        *g_dt_file_stream << "\n[DocToText] Parsing: " << file_path << " (ext: " << lower_ext << ")\n" << std::flush;
      }
    }

    std::unique_ptr<DocToTextExtractorParams, decltype(&doctotext_free_extractor_params)> params(
        doctotext_create_extractor_params(), &doctotext_free_extractor_params);
    if (!params) {
      if (out_dt_status) *out_dt_status = DOCTOTEXT_STATUS_ERROR;
      if (out_dt_error) *out_dt_error = "Failed to allocate DocToText extractor parameters";
      return;
    }

    doctotext_extractor_params_set_verbose_logging(params.get(), enable_logging ? 1 : 0);
    if (enable_logging && !log_file_path.empty()) {
      const uint64_t seq = g_dt_tmp_seq.fetch_add(1, std::memory_order_relaxed);
      temp_cleaner.path =
          log_file_path + ".dt." + std::to_string(CurrentProcessId()) + "." + std::to_string(seq) + ".tmp";
      doctotext_extractor_params_set_log_file(params.get(), temp_cleaner.path.c_str());
    } else {
      // DocToText defaults to stderr even with verbose=0; redirect to null device when disabled.
      doctotext_extractor_params_set_log_file(params.get(), kNullDevice);
    }
    doctotext_extractor_params_set_parser_type(params.get(), ResolveDocToTextParserType(lower_ext));
    if (s_embedded_image_ocr.load(std::memory_order_relaxed)) {
      doctotext_extractor_params_set_image_parser_callback(params.get(), &L2OfficeParser::DocToTextImageParserCallbackC,
                                                           &local_budget);
    } else {
      doctotext_extractor_params_set_image_parser_callback(params.get(), nullptr, nullptr);
    }

    DocToTextException *raw_exc = nullptr;
    doctotext_status dt_status = DOCTOTEXT_STATUS_OK;
    std::string native_path = common::EncodingUtils::ToNativeAnsiPath(file_path);
    std::unique_ptr<DocToTextExtractedData, decltype(&doctotext_free_extracted_data)> data(
        doctotext_process_file_ex(native_path.c_str(), params.get(), &dt_status, &raw_exc),
        &doctotext_free_extracted_data);
    std::unique_ptr<DocToTextException, decltype(&doctotext_free_exception)> exc(raw_exc, &doctotext_free_exception);

    if (data) {
      const char *txt = doctotext_extracted_data_get_text(data.get());
      if (txt && *txt) {
        *out_utf8_text = txt;
      }
      if (out_dt_status) *out_dt_status = DOCTOTEXT_STATUS_OK;
    } else {
      std::string dt_err = ExtractDocToTextErrors(exc.get());
      if (dt_status == DOCTOTEXT_STATUS_ENCRYPTED) {
        if (out_dt_status) *out_dt_status = DOCTOTEXT_STATUS_ENCRYPTED;
        if (out_dt_error) {
          *out_dt_error = dt_err.empty() ? "Document is encrypted and requires a password" : std::move(dt_err);
        }
      } else {
        if (out_dt_status) *out_dt_status = DOCTOTEXT_STATUS_ERROR;
        if (out_dt_error) *out_dt_error = std::move(dt_err);
      }
    }

    if (!temp_cleaner.path.empty()) {
      std::error_code ec;
      bool exists = std::filesystem::exists(temp_cleaner.path, ec);
      if (!ec && exists) {
        std::ifstream tmp_ifs(temp_cleaner.path);
        if (tmp_ifs.is_open()) {
          std::string dt_logs((std::istreambuf_iterator<char>(tmp_ifs)), std::istreambuf_iterator<char>());
          tmp_ifs.close();
          std::lock_guard<std::mutex> lock(g_dt_mutex);
          if (g_dt_file_stream && g_dt_file_stream->is_open()) {
            *g_dt_file_stream << dt_logs << std::flush;
          }
        }
      }
    }

    if (enable_logging) {
      std::lock_guard<std::mutex> lock(g_dt_mutex);
      if (g_dt_file_stream && g_dt_file_stream->is_open()) {
        g_dt_file_stream->flush();
      }
    }
    if (local_budget.hit_limit) {
      LOG_WARN(
          "[OfficeParser] Embedded OCR 3/4 budget reached ({} ms) for '{}': processed {} images, skipped {} remaining "
          "images (document text preserved).",
          local_budget.budget_ms, file_path, local_budget.processed_count, local_budget.skipped_count);
    }
  } catch (const std::exception &e) {
    LOG_WARN("[OfficeParser] ExtractDocToText failed for {}: {}", file_path, e.what());
    if (out_dt_status) *out_dt_status = DOCTOTEXT_STATUS_ERROR;
    if (out_dt_error && out_dt_error->empty()) *out_dt_error = e.what();
    // Graceful fallback to secondary extractors
  } catch (...) {
    LOG_WARN("[OfficeParser] ExtractDocToText failed for {} with unknown exception", file_path);
    if (out_dt_status) *out_dt_status = DOCTOTEXT_STATUS_ERROR;
    if (out_dt_error && out_dt_error->empty()) *out_dt_error = "DocToText unknown internal exception";
    // Graceful fallback to secondary extractors
  }
  if (out_budget) {
    *out_budget = local_budget;
  }
}

void L2OfficeParser::CheckIdleUnload(double idle_seconds) {
  std::lock_guard<std::mutex> lock(ocr_mutex_);
  if (!ocr_engine_) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - last_embedded_ocr_active_time_).count();
  if (elapsed >= idle_seconds) {
    ocr_engine_.reset();
    LOG_INFO("[OfficeParser] Embedded OCR engine idle for {:.1f}s. Unloaded from memory to reclaim RAM.", elapsed);
    common::SystemEnv::TrimHeap();
  }
}

bool L2OfficeParser::is_embedded_ocr_loaded() const {
  std::lock_guard<std::mutex> lock(ocr_mutex_);
  return ocr_engine_ != nullptr;
}

bool L2OfficeParser::IsSupported(const std::string &ext) const {
  return supported_exts_.find(ToLowerAscii(ext)) != supported_exts_.end();
}

void L2OfficeParser::SetLogging(bool enable, const std::string &log_file_path) {
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

bool L2OfficeParser::ParseInternal(const std::string &file_path, const std::string &ext, uint32_t timeout_ms,
                                 ExtractedDocument *out_doc) {
  if (!out_doc) return false;
  out_doc->utf8_text.clear();
  out_doc->is_scanned_pdf = false;
  out_doc->outcome = ParseOutcome::kOk;
  out_doc->error_message.clear();

  const std::string lower = ToLowerAscii(ext);

  // 1. Primary: Use DocToText (lithiumFlower/doctotext) for comprehensive multi-format extraction
  int dt_status = DOCTOTEXT_STATUS_OK;
  std::string dt_error;
  EmbeddedOcrBudget ocr_budget;
  ExtractWithDocToText(file_path, lower, timeout_ms, &out_doc->utf8_text, &dt_status, &dt_error, &ocr_budget);

  // Never fall back to raw stream/ZIP scanners on encrypted documents (avoids scanned-PDF false positives).
  if (dt_status == DOCTOTEXT_STATUS_ENCRYPTED) {
    out_doc->utf8_text.clear();
    out_doc->is_scanned_pdf = false;
    out_doc->outcome = ParseOutcome::kEncrypted;
    out_doc->error_message =
        dt_error.empty() ? "Document is encrypted and requires a password" : std::move(dt_error);
    return false;
  }

  // 2. Secondary / Fallback: Only run structured OOXML/PDF fallback when DocToText reported an error.
  //    If DocToText returned DOCTOTEXT_STATUS_OK with empty/whitespace text, blank PDFs stay kOk while
  //    empty valid Office/RTF documents are classified below as kEmptyContent.
  if (dt_status != DOCTOTEXT_STATUS_OK && !HasMeaningfulText(out_doc->utf8_text)) {
    ApplyFallbackExtractor(file_path, lower, &out_doc->utf8_text, &out_doc->is_scanned_pdf);
  }

  // 3. Sanitize UTF-8 output before counting Unicode codepoints
  if (!out_doc->utf8_text.empty()) {
    std::string sanitized;
    sanitized.reserve(out_doc->utf8_text.size());
    utf8::replace_invalid(out_doc->utf8_text.begin(), out_doc->utf8_text.end(), std::back_inserter(sanitized), ' ');
    out_doc->utf8_text = std::move(sanitized);
  }

  // 4. Flag scanned / image-only PDFs that still lack meaningful text (> 10 KB & < 15 codepoints)
  if (lower == "pdf") {
    const size_t non_ws_codepoints = CountNonWhitespaceCodepoints(out_doc->utf8_text);
    const uint64_t file_size = GetFileSize(file_path);
    if (non_ws_codepoints < 15 && file_size > 10 * 1024) {
      out_doc->is_scanned_pdf = true;
    }
  }

  auto build_budget_msg = [&ocr_budget](bool has_text) {
    return "Embedded image OCR budget (" + std::to_string(ocr_budget.budget_ms) + " ms) reached after " +
           std::to_string(ocr_budget.processed_count) + " images; skipped " +
           std::to_string(ocr_budget.skipped_count) + " images (" +
           (has_text ? "document text preserved)" : "no document text extracted)");
  };

  const bool has_text = HasMeaningfulText(out_doc->utf8_text);
  if (has_text || (lower == "pdf" && (out_doc->is_scanned_pdf || dt_status == DOCTOTEXT_STATUS_OK))) {
    if (ocr_budget.hit_limit) {
      out_doc->outcome = ParseOutcome::kPartialOcr;
      out_doc->error_message = build_budget_msg(has_text);
    } else {
      out_doc->outcome = ParseOutcome::kOk;
    }
    return true;
  }

  if (dt_status == DOCTOTEXT_STATUS_OK && !ocr_budget.hit_limit) {
    out_doc->utf8_text.clear();
    out_doc->outcome = ParseOutcome::kEmptyContent;
    out_doc->error_message =
        "Document contains no extractable text (empty body, images without text, macros, or links only)";
    return true;
  }

  out_doc->outcome = ParseOutcome::kFailed;
  if (!dt_error.empty()) {
    out_doc->error_message = std::move(dt_error);
  } else if (ocr_budget.hit_limit) {
    out_doc->error_message = build_budget_msg(false);
  } else {
    out_doc->error_message = "Failed to extract text from document (empty or unsupported structure)";
  }
  return false;
}

bool L2OfficeParser::Parse(const std::string &file_path, const std::string &ext, std::string *out_utf8_text,
                           bool *is_scanned_pdf) {
  if (!out_utf8_text) return false;
  out_utf8_text->clear();
  if (is_scanned_pdf) *is_scanned_pdf = false;

  ExtractedDocument doc;
  const bool ok = ParseInternal(file_path, ext, 60000, &doc);
  *out_utf8_text = std::move(doc.utf8_text);
  if (is_scanned_pdf) *is_scanned_pdf = doc.is_scanned_pdf;
  return ok && doc.outcome != ParseOutcome::kEmptyContent;
}

bool L2OfficeParser::ParseDocx(const std::string &file_path, std::string *out_utf8) {
  ZipFileScope zip_scope;
  if (!zip_scope.Open(file_path)) {
    return false;
  }
  *out_utf8 = ExtractZipEntryXmlText(zip_scope.zip(), "word/document.xml", true);
  if (out_utf8->empty()) {
    const std::string alt_path = ResolveAlternateDocxPath(zip_scope.zip());
    if (!alt_path.empty()) {
      *out_utf8 = ExtractZipEntryXmlText(zip_scope.zip(), alt_path.c_str(), true);
    }
  }
  return !out_utf8->empty();
}

bool L2OfficeParser::ParseXlsx(const std::string &file_path, std::string *out_utf8) {
  ZipFileScope zip_scope;
  if (!zip_scope.Open(file_path)) {
    return false;
  }
  mz_zip_archive *zip = zip_scope.zip();

  std::ostringstream oss;

  // 1. Extract sharedStrings.xml (primary string table in XLSX)
  std::string shared_text = ExtractZipEntryXmlText(zip, "xl/sharedStrings.xml", true);
  if (!shared_text.empty()) {
    oss << shared_text << "\n";
  }

  // 2. Extract worksheet cell contents
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(zip); ++i) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(zip, i, &file_stat)) continue;

    std::string filename(file_stat.m_filename);
    if (filename.rfind("xl/worksheets/sheet", 0) == 0 && filename.size() >= 4 &&
        filename.substr(filename.size() - 4) == ".xml") {
      std::string sheet_text = ExtractZipEntryXmlText(zip, file_stat.m_filename, false);
      if (!sheet_text.empty()) {
        oss << sheet_text << "\n";
      }
    }
  }

  *out_utf8 = oss.str();
  return !out_utf8->empty();
}

bool L2OfficeParser::ParsePptx(const std::string &file_path, std::string *out_utf8) {
  ZipFileScope zip_scope;
  if (!zip_scope.Open(file_path)) {
    return false;
  }
  mz_zip_archive *zip = zip_scope.zip();

  std::ostringstream oss;

  // Extract all slide*.xml
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(zip); ++i) {
    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(zip, i, &file_stat)) continue;

    std::string filename(file_stat.m_filename);
    if (filename.rfind("ppt/slides/slide", 0) == 0 && filename.size() >= 4 &&
        filename.substr(filename.size() - 4) == ".xml") {
      std::string slide_text = ExtractZipEntryXmlText(zip, file_stat.m_filename, true);
      if (!slide_text.empty()) {
        oss << slide_text << "\n";
      }
    }
  }

  *out_utf8 = oss.str();
  return !out_utf8->empty();
}

bool L2OfficeParser::ParsePdf(const std::string &file_path, std::string *out_utf8, bool *is_scanned) {
  if (is_scanned) *is_scanned = false;
  if (!out_utf8) return false;
  out_utf8->clear();

  // Read file stream to extract text in BT ... ET blocks and string literals
  auto ifs = common::EncodingUtils::OpenIfstream(file_path, std::ios::binary);
  if (!ifs.is_open()) {
    return false;
  }

  std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  ifs.close();

  // 1. Search for text objects in PDF streams: BT ... ET (ISO 32000-1)
  std::string extracted;
  size_t pos = 0;
  while ((pos = content.find("BT", pos)) != std::string::npos) {
    // Ensure BT is an isolated token
    bool valid_bt = (pos == 0 || std::isspace(static_cast<unsigned char>(content[pos - 1]))) &&
                    (pos + 2 >= content.size() || std::isspace(static_cast<unsigned char>(content[pos + 2])));
    if (!valid_bt) {
      pos += 2;
      continue;
    }

    size_t end_pos = content.find("ET", pos + 2);
    if (end_pos == std::string::npos) break;

    std::string bt_block = content.substr(pos + 2, end_pos - (pos + 2));
    // Extract strings in parentheses: ( ... )
    size_t str_pos = 0;
    while ((str_pos = bt_block.find('(', str_pos)) != std::string::npos) {
      size_t str_end = str_pos + 1;
      int depth = 1;
      while (str_end < bt_block.size() && depth > 0) {
        if (bt_block[str_end] == '\\') {
          str_end += 2;
          continue;
        }
        if (bt_block[str_end] == '(') {
          depth++;
        } else if (bt_block[str_end] == ')') {
          depth--;
        }
        str_end++;
      }
      if (depth == 0) {
        std::string raw_str = bt_block.substr(str_pos + 1, str_end - str_pos - 2);
        if (!raw_str.empty()) {
          extracted += common::EncodingUtils::ToUtf8(raw_str) + " ";
        }
      }
      str_pos = str_end;
    }
    pos = end_pos + 2;
  }

  // If no selectable BT...ET text found, check if it is a scanned image PDF (>10KB)
  uint64_t file_size = GetFileSize(file_path);
  if (extracted.empty()) {
    if (file_size > 10 * 1024) {
      if (is_scanned) *is_scanned = true;
    }
  } else {
    *out_utf8 = std::move(extracted);
  }

  return !out_utf8->empty() || (is_scanned && *is_scanned);
}

std::string L2OfficeParser::ExtractXmlText(const std::string &xml_content, bool insert_newlines) {
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
        if (tag_name == "/w:p" || tag_name == "w:p" || tag_name == "/a:p" || tag_name == "a:p" || tag_name == "w:br" ||
            tag_name == "a:br" || tag_name == "/row" || tag_name == "/tr") {
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

std::string L2OfficeParser::UnescapeXmlEntities(const std::string &text) {
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

bool L2OfficeParser::Parse(const ParseContext &ctx, ExtractedDocument *out_doc) {
  if (!out_doc) {
    return false;
  }
  out_doc->page_count = 1;
  const uint32_t effective_timeout_ms = ctx.timeout_ms > 0 ? ctx.timeout_ms : 60000;
  return ParseInternal(ctx.file_path, ctx.ext, effective_timeout_ms, out_doc);
}

}  // namespace worker
}  // namespace soeasy
