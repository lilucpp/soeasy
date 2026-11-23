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

/// Helper to safely extract and parse an XML entry from a zip archive using RAII.
std::string ExtractZipEntryXmlText(mz_zip_archive *zip, const char *entry_name, bool strip_tags) {
  size_t uncomp_size = 0;
  void *data = mz_zip_reader_extract_file_to_heap(zip, entry_name, &uncomp_size, 0);
  if (!data) {
    return "";
  }
  std::unique_ptr<char, decltype(&mz_free)> data_guard(static_cast<char *>(data), mz_free);
  std::string xml(data_guard.get(), uncomp_size);
  return L2OfficeParser::ExtractXmlText(xml, strip_tags);
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

/// Dispatch secondary extractors when DocToText yields empty or whitespace-only text.
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
  } else if (lower_ext == "doc" || lower_ext == "xls" || lower_ext == "ppt" || lower_ext == "wps" ||
             lower_ext == "et" || lower_ext == "dps" || lower_ext == "rtf") {
    ok = L2OfficeParser::ParseLegacyOffice(file_path, lower_ext, out_utf8_text);
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

std::string L2OfficeParser::RunEmbeddedImageOcr(const std::string &image_name, const char *image_buffer,
                                                size_t image_size) {
  if (!image_buffer || image_size < 128 || image_size > 30ULL * 1024ULL * 1024ULL) return "";

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
    if (!ocr_engine_) return "";
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
  auto *self = static_cast<L2OfficeParser *>(user_data);
  static thread_local std::string tls_ocr_result;
  tls_ocr_result = self->RunEmbeddedImageOcr(image_name ? image_name : "", image_buffer, image_size);
  return tls_ocr_result.empty() ? nullptr : tls_ocr_result.c_str();
}

void L2OfficeParser::ExtractWithDocToText(const std::string &file_path, const std::string &lower_ext,
                                          std::string *out_utf8_text, int *out_dt_status, std::string *out_dt_error) {
  if (out_dt_status) *out_dt_status = DOCTOTEXT_STATUS_OK;
  if (out_dt_error) out_dt_error->clear();
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
                                                           this);
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

bool L2OfficeParser::ParseInternal(const std::string &file_path, const std::string &ext, ExtractedDocument *out_doc) {
  if (!out_doc) return false;
  out_doc->utf8_text.clear();
  out_doc->is_scanned_pdf = false;
  out_doc->outcome = ParseOutcome::kOk;
  out_doc->error_message.clear();

  const std::string lower = ToLowerAscii(ext);

  // 1. Primary: Use DocToText (lithiumFlower/doctotext) for comprehensive multi-format extraction
  int dt_status = DOCTOTEXT_STATUS_OK;
  std::string dt_error;
  ExtractWithDocToText(file_path, lower, &out_doc->utf8_text, &dt_status, &dt_error);

  // Never fall back to raw stream/ZIP scanners on encrypted documents (avoids scanned-PDF false positives).
  if (dt_status == DOCTOTEXT_STATUS_ENCRYPTED) {
    out_doc->utf8_text.clear();
    out_doc->is_scanned_pdf = false;
    out_doc->outcome = ParseOutcome::kEncrypted;
    out_doc->error_message =
        dt_error.empty() ? "Document is encrypted and requires a password" : std::move(dt_error);
    return false;
  }

  // 2. Secondary / Fallback: If DocToText extraction was empty or purely whitespace, use internal extractors
  if (!HasMeaningfulText(out_doc->utf8_text)) {
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

  if (!out_doc->utf8_text.empty() || (lower == "pdf" && out_doc->is_scanned_pdf)) {
    out_doc->outcome = ParseOutcome::kOk;
    return true;
  }

  out_doc->outcome = ParseOutcome::kFailed;
  out_doc->error_message =
      !dt_error.empty() ? std::move(dt_error) : "Failed to extract text from document (empty or unsupported structure)";
  return false;
}

bool L2OfficeParser::Parse(const std::string &file_path, const std::string &ext, std::string *out_utf8_text,
                           bool *is_scanned_pdf) {
  if (!out_utf8_text) return false;
  out_utf8_text->clear();
  if (is_scanned_pdf) *is_scanned_pdf = false;

  ExtractedDocument doc;
  const bool ok = ParseInternal(file_path, ext, &doc);
  *out_utf8_text = std::move(doc.utf8_text);
  if (is_scanned_pdf) *is_scanned_pdf = doc.is_scanned_pdf;
  return ok;
}

bool L2OfficeParser::ParseDocx(const std::string &file_path, std::string *out_utf8) {
  ZipFileScope zip_scope;
  if (!zip_scope.Open(file_path)) {
    return false;
  }
  *out_utf8 = ExtractZipEntryXmlText(zip_scope.zip(), "word/document.xml", true);
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

bool L2OfficeParser::ParseLegacyOffice(const std::string &file_path, const std::string &ext, std::string *out_utf8) {
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
    if (wc >= 0x00A0 && wc < 0xD800) return true;  // Unicode (including CJK 0x4E00 - 0x9FFF)
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
  return ParseInternal(ctx.file_path, ctx.ext, out_doc);
}

}  // namespace worker
}  // namespace soeasy
