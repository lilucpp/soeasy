// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "common/encoding_utils.h"
#include "common/logger.h"
#include "common/machine_key_provider.h"
#include "common/parse_status.h"
#include "common/snippet_highlighter.h"
#include "service/name_index.h"
#include "service/search_coordinator.h"
#include "storage/storage_engine.h"

namespace fs = std::filesystem;

namespace {

void PrintUsage(const char *prog_name) {
  std::cerr << "SoEasy Desktop Search Engine - Command Line Interface\n\n"
            << "Usage:\n"
            << "  " << prog_name << " <command> [options]\n\n"
            << "Commands:\n"
            << "  search <query>      Search indexed documents by keywords\n"
            << "  stats               Display storage and index statistics\n"
            << "  index <path>        Scan and index a directory or file\n"
            << "  help                Show this help message\n\n"
            << "Global Options:\n"
            << "  -d, --db <path>     Path to SQLite3MC encrypted database\n"
            << "      --key <key>     Database encryption key (default: machine-bound DEK)\n"
            << "      --dict <path>   Path to Jieba dictionary directory\n"
            << "  -v, --verbose       Enable verbose diagnostic output to stderr\n"
            << "  -h, --help          Show help message\n\n"
            << "Search Options:\n"
            << "  -l, --limit <n>     Maximum number of results to return (default: 50)\n"
            << "  -o, --offset <n>    Pagination offset (default: 0)\n"
            << "  -p, --path <prefix> Filter results under a directory prefix\n"
            << "      --json          Output results in valid JSON format\n"
            << "      --snippet       Include extracted text snippets in results\n\n"
            << "Examples:\n"
            << "  " << prog_name << " search \"algorithm\" --limit 10\n"
            << "  " << prog_name << " search \"report\" --json\n"
            << "  " << prog_name << " stats --json\n";
}

std::string EscapeJson(const std::string &input) {
  std::ostringstream ss;
  for (char c : input) {
    switch (c) {
      case '"':
        ss << "\\\"";
        break;
      case '\\':
        ss << "\\\\";
        break;
      case '\b':
        ss << "\\b";
        break;
      case '\f':
        ss << "\\f";
        break;
      case '\n':
        ss << "\\n";
        break;
      case '\r':
        ss << "\\r";
        break;
      case '\t':
        ss << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
        } else {
          ss << c;
        }
    }
  }
  return ss.str();
}

std::string ResolveAppDataDir() {
  std::error_code ec;
#ifdef _WIN32
  fs::path base_dir;
  if (const char *appdata = std::getenv("APPDATA"); appdata && *appdata) {
    base_dir = fs::path(appdata);
  } else if (const char *profile = std::getenv("USERPROFILE"); profile && *profile) {
    base_dir = fs::path(profile) / "AppData" / "Roaming";
  } else {
    return ".soeasy";
  }
  fs::path gui_dir = base_dir / "SoEasy";
  fs::path legacy_cli_dir;
  if (const char *local_appdata = std::getenv("LOCALAPPDATA"); local_appdata && *local_appdata) {
    legacy_cli_dir = fs::path(local_appdata) / "soeasy";
  }
  if (!fs::exists(gui_dir / "soeasy_index.db", ec) && !legacy_cli_dir.empty() &&
      fs::exists(legacy_cli_dir / "soeasy_index.db", ec)) {
    return soeasy::common::EncodingUtils::PathToUtf8(legacy_cli_dir);
  }
  return soeasy::common::EncodingUtils::PathToUtf8(gui_dir);
#else
  fs::path data_root;
  const char *xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && *xdg) {
    data_root = fs::path(xdg);
  } else if (const char *home = std::getenv("HOME"); home && *home) {
    data_root = fs::path(home) / ".local" / "share";
  } else {
    return ".soeasy";
  }
  fs::path gui_dir = data_root / "SoEasy";
  fs::path legacy_cli_dir = data_root / "soeasy";
  if (!fs::exists(gui_dir / "soeasy_index.db", ec) && fs::exists(legacy_cli_dir / "soeasy_index.db", ec)) {
    return legacy_cli_dir.string();
  }
  return gui_dir.string();
#endif
}

fs::path ResolveExecutableDir() {
  std::error_code ec;
#ifdef _WIN32
  std::vector<wchar_t> exe_buf(MAX_PATH);
  while (exe_buf.size() <= 32768) {
    DWORD len = GetModuleFileNameW(nullptr, exe_buf.data(), static_cast<DWORD>(exe_buf.size()));
    if (len == 0) {
      break;
    }
    if (len < exe_buf.size()) {
      return fs::path(exe_buf.data()).parent_path();
    }
    exe_buf.resize(exe_buf.size() * 2);
  }
#elif defined(__linux__)
  fs::path exe_path = fs::read_symlink("/proc/self/exe", ec);
  if (!ec && !exe_path.empty()) {
    return exe_path.parent_path();
  }
#endif
  return {};
}

std::string FindDefaultDictDir() {
  std::vector<fs::path> candidates;
  fs::path exe_dir = ResolveExecutableDir();
  if (!exe_dir.empty()) {
    candidates.push_back(exe_dir / "dict");
    candidates.push_back(exe_dir / ".." / "dict");
    candidates.push_back(exe_dir / ".." / "third_party" / "cppjieba" / "dict");
    candidates.push_back(exe_dir / ".." / ".." / "third_party" / "cppjieba" / "dict");
  }

  candidates.emplace_back("/usr/local/share/soeasy/dict");
  candidates.emplace_back("/usr/share/soeasy/dict");
  candidates.emplace_back("./dict");
  candidates.emplace_back("../dict");
  candidates.emplace_back("./third_party/cppjieba/dict");
  candidates.emplace_back("../third_party/cppjieba/dict");

  std::error_code ec;
  for (const auto &c : candidates) {
    if (fs::exists(c / "jieba_dat.bin", ec) || fs::exists(c / "jieba.dict.utf8", ec)) {
      fs::path canonical_dir = fs::weakly_canonical(c, ec);
      return soeasy::common::EncodingUtils::PathToUtf8(ec ? c : canonical_dir);
    }
  }
  return "./dict";
}

int HandleSearch(soeasy::storage::StorageEngine &storage, const std::string &query, int limit, int offset,
                 const std::string &path_prefix, bool json_output, bool show_snippet) {
  if (query.empty()) {
    std::cerr << "[-] Error: Search query cannot be empty.\n";
    return 2;
  }

  int total_count = 0;
  auto results = storage.Search(query, limit, offset, path_prefix, &total_count);
  auto keywords = soeasy::common::SnippetHighlighter::ExtractKeywordsFromQuery(query);

  if (json_output) {
    std::cout << "{\n"
              << "  \"query\": \"" << EscapeJson(query) << "\",\n"
              << "  \"total_count\": " << total_count << ",\n"
              << "  \"offset\": " << offset << ",\n"
              << "  \"limit\": " << limit << ",\n"
              << "  \"results\": [\n";

    for (size_t i = 0; i < results.size(); ++i) {
      const auto &r = results[i];
      std::cout << "    {\n"
                << "      \"doc_id\": " << r.doc_id << ",\n"
                << "      \"file_name\": \"" << EscapeJson(r.file_name) << "\",\n"
                << "      \"file_path\": \"" << EscapeJson(r.file_path) << "\",\n"
                << "      \"ext\": \"" << EscapeJson(r.ext) << "\",\n"
                << "      \"file_size\": " << r.file_size << ",\n"
                << "      \"mtime\": " << r.mtime << ",\n"
                << "      \"rank\": " << r.rank;

      if (show_snippet) {
        std::string content;
        std::string snippet;
        if (storage.GetDocumentContent(r.doc_id, &content)) {
          snippet = soeasy::common::SnippetHighlighter::GenerateSnippet(content, keywords, 120);
        }
        std::cout << ",\n      \"snippet\": \"" << EscapeJson(snippet) << "\"\n";
      } else {
        std::cout << "\n";
      }

      std::cout << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    std::cout << "  ]\n}\n";
  } else {
    if (results.empty()) {
      std::cerr << "No matching documents found for: \"" << query << "\"\n";
      return 1;
    }

    std::cout << "Found " << total_count << " matches (showing " << results.size() << "):\n\n";
    for (size_t i = 0; i < results.size(); ++i) {
      const auto &r = results[i];
      std::cout << "[" << (offset + i + 1) << "] " << r.file_name << "\n"
                << "    Path: " << r.file_path << "\n"
                << "    Size: " << r.file_size << " bytes | Score: " << r.rank << "\n";

      if (show_snippet) {
        std::string content;
        if (storage.GetDocumentContent(r.doc_id, &content)) {
          std::string snippet = soeasy::common::SnippetHighlighter::GenerateSnippet(content, keywords, 120);
          if (!snippet.empty()) {
            std::cout << "    Snippet: " << snippet << "\n";
          }
        }
      }
      std::cout << "\n";
    }
  }

  return results.empty() ? 1 : 0;
}

int HandleStats(soeasy::storage::StorageEngine &storage, bool json_output, bool verbose) {
  soeasy::storage::StorageFragmentation frag;
  bool frag_ok = storage.GetFragmentation(&frag);

  int64_t total_docs = 0;
  uint64_t total_indexed_bytes = 0;
  storage.ForEachFileMetadata([&](const soeasy::storage::FileMetaRecord &r) {
    total_docs++;
    total_indexed_bytes += r.file_size;
  });

  const soeasy::common::ParseStatusSummary parse_summary = storage.GetParseStatusSummary();
  const size_t issue_limit = (json_output || verbose) ? 5000 : 20;
  const auto issues = storage.GetParseIssues(std::nullopt, issue_limit, 0);

  if (json_output) {
    std::cout << "{\n"
              << "  \"total_documents\": " << total_docs << ",\n"
              << "  \"normal_documents\": " << parse_summary.ok_count << ",\n"
              << "  \"encrypted_documents\": " << parse_summary.encrypted_count << ",\n"
              << "  \"partial_ocr_documents\": " << parse_summary.partial_ocr_count << ",\n"
              << "  \"empty_content_documents\": " << parse_summary.empty_content_count << ",\n"
              << "  \"timeout_documents\": " << parse_summary.timeout_count << ",\n"
              << "  \"crashed_documents\": " << parse_summary.crashed_count << ",\n"
              << "  \"failed_documents\": " << parse_summary.failed_count << ",\n"
              << "  \"total_indexed_bytes\": " << total_indexed_bytes << ",\n"
              << "  \"db_page_size\": " << (frag_ok ? frag.page_size : 4096) << ",\n"
              << "  \"db_total_pages\": " << (frag_ok ? frag.total_pages : 0) << ",\n"
              << "  \"db_free_pages\": " << (frag_ok ? frag.free_pages : 0) << ",\n"
              << "  \"db_total_bytes\": " << (frag_ok ? frag.total_bytes : 0) << ",\n"
              << "  \"db_free_bytes\": " << (frag_ok ? frag.free_bytes : 0) << ",\n"
              << "  \"db_fragmentation_ratio\": " << (frag_ok ? frag.frag_ratio : 0.0) << ",\n"
              << "  \"issues\": [\n";
    for (size_t i = 0; i < issues.size(); ++i) {
      const auto &item = issues[i];
      std::cout << "    {\n"
                << "      \"status\": \"" << soeasy::common::ParseStatusToKey(item.status) << "\",\n"
                << "      \"file_name\": \"" << EscapeJson(item.file_name) << "\",\n"
                << "      \"file_path\": \"" << EscapeJson(item.file_path) << "\",\n"
                << "      \"ext\": \"" << EscapeJson(item.ext) << "\",\n"
                << "      \"file_size\": " << item.file_size << ",\n"
                << "      \"reason\": \"" << EscapeJson(item.reason) << "\"\n"
                << "    }" << (i + 1 < issues.size() ? "," : "") << "\n";
    }
    std::cout << "  ]\n"
              << "}\n";
  } else {
    std::cout << "========================================================\n"
              << "   SoEasy Storage & Index Statistics\n"
              << "========================================================\n"
              << "Total Documents Indexed: " << total_docs << "\n"
              << "Normal Parsed:           " << parse_summary.ok_count << "\n"
              << "Encrypted (Skipped):     " << parse_summary.encrypted_count << "\n"
              << "Partial OCR:             " << parse_summary.partial_ocr_count << "\n"
              << "Empty Content:           " << parse_summary.empty_content_count << "\n"
              << "Failures (Total):        " << parse_summary.failure_count()
              << " (timeout: " << parse_summary.timeout_count << ", crashed: " << parse_summary.crashed_count
              << ", failed: " << parse_summary.failed_count << ")\n"
              << "Total Raw Data Size:     " << (total_indexed_bytes / 1024 / 1024) << " MB (" << total_indexed_bytes
              << " bytes)\n";
    if (frag_ok) {
      std::cout << "Database File Size:      " << (frag.total_bytes / 1024 / 1024) << " MB (" << frag.total_bytes
                << " bytes)\n"
                << "Database Free Space:     " << (frag.free_bytes / 1024 / 1024) << " MB (" << frag.free_bytes
                << " bytes)\n"
                << "Fragmentation Ratio:     " << std::fixed << std::setprecision(2) << (frag.frag_ratio * 100.0)
                << " %\n";
    }
    if (!issues.empty()) {
      std::cout << "\nParse Issues (" << parse_summary.issue_count() << " total"
                << (issues.size() < parse_summary.issue_count() ? ", showing first " + std::to_string(issues.size())
                                                                : "")
                << "):\n";
      for (const auto &item : issues) {
        std::cout << "  [" << soeasy::common::ParseStatusToKey(item.status) << "] " << item.file_path;
        if (!item.reason.empty()) {
          std::cout << "  -- " << item.reason;
        }
        std::cout << "\n";
      }
      if (!verbose && issues.size() < parse_summary.issue_count()) {
        std::cout << "  ... run with --verbose (-v) or --json to view all issues.\n";
      }
    }
  }

  return 0;
}

int HandleIndex(soeasy::storage::StorageEngine &storage, const std::string &target_path, bool verbose) {
  if (target_path.empty()) {
    std::cerr << "[-] Error: Missing target path for indexing.\n";
    return 2;
  }

  if (!fs::exists(target_path)) {
    std::cerr << "[-] Error: Target path does not exist: " << target_path << "\n";
    return 1;
  }

  std::cout << "[+] Scanning target: " << target_path << "...\n";
  size_t count = 0;
  std::vector<fs::path> files_to_index;

  if (fs::is_regular_file(target_path)) {
    files_to_index.push_back(target_path);
  } else if (fs::is_directory(target_path)) {
    try {
      for (const auto &entry :
           fs::recursive_directory_iterator(target_path, fs::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file()) {
          files_to_index.push_back(entry.path());
        }
      }
    } catch (const std::exception &ex) {
      std::cerr << "[-] Directory walk error: " << ex.what() << "\n";
    }
  }

  std::cout << "[+] Found " << files_to_index.size() << " files to evaluate.\n";
  for (const auto &p : files_to_index) {
    std::string path_str = p.string();
    std::string filename = p.filename().string();
    std::string ext = p.has_extension() ? p.extension().string() : "";
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);

    uint64_t file_size = 0;
    int64_t mtime = 0;
    try {
      file_size = fs::file_size(p);
      auto ftime = fs::last_write_time(p);
      auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
      mtime = std::chrono::system_clock::to_time_t(sctp);
    } catch (const std::exception &ex) {
      if (verbose) {
        std::cerr << "[-] Error reading metadata for " << path_str << ": " << ex.what() << "\n";
      }
    }

    // Quick text reading for plain text files
    std::string content;
    if (ext == "txt" || ext == "md" || ext == "cpp" || ext == "h" || ext == "py" || ext == "json") {
      std::ifstream ifs(path_str, std::ios::binary);
      if (ifs) {
        content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      }
    }

    soeasy::storage::DocumentIndexEntry entry;
    entry.doc_id = 0;
    entry.file_path = path_str;
    entry.file_name = filename;
    entry.ext = ext;
    entry.file_size = file_size;
    entry.mtime = mtime;
    entry.parse_level = 1;
    entry.utf8_content = content;

    if (storage.IndexDocument(entry)) {
      count++;
    }
  }

  std::cout << "[+] Successfully indexed " << count << " documents.\n";
  return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 2;
  }

  std::string command;
  std::string query;
  std::string index_path;
  std::string db_path;
  std::string db_key;
  std::string dict_dir;
  int limit = 50;
  int offset = 0;
  std::string path_prefix;
  bool json_output = false;
  bool show_snippet = false;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help" || arg == "help") {
      PrintUsage(argv[0]);
      return 0;
    } else if (arg == "-v" || arg == "--verbose") {
      verbose = true;
    } else if (arg == "--json") {
      json_output = true;
    } else if (arg == "--snippet") {
      show_snippet = true;
    } else if (arg == "-d" || arg == "--db") {
      if (i + 1 < argc) db_path = argv[++i];
    } else if (arg == "--key") {
      if (i + 1 < argc) db_key = argv[++i];
    } else if (arg == "--dict") {
      if (i + 1 < argc) dict_dir = argv[++i];
    } else if (arg == "-l" || arg == "--limit") {
      if (i + 1 < argc) {
        std::string val = argv[++i];
        int n = 0;
        auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), n);
        if (ec == std::errc()) limit = n;
      }
    } else if (arg == "-o" || arg == "--offset") {
      if (i + 1 < argc) {
        std::string val = argv[++i];
        int n = 0;
        auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), n);
        if (ec == std::errc()) offset = n;
      }
    } else if (arg == "-p" || arg == "--path") {
      if (i + 1 < argc) path_prefix = argv[++i];
    } else if (command.empty()) {
      command = arg;
    } else if (command == "search" && query.empty()) {
      query = arg;
    } else if (command == "index" && index_path.empty()) {
      index_path = arg;
    }
  }

  if (command.empty()) {
    PrintUsage(argv[0]);
    return 2;
  }

  std::string app_data_dir = ResolveAppDataDir();
  std::error_code ec_dir;
  fs::create_directories(app_data_dir, ec_dir);
  if (db_path.empty()) {
    db_path = app_data_dir + "/soeasy_index.db";
  }
  fs::create_directories(fs::path(db_path).parent_path(), ec_dir);
  fs::create_directories(app_data_dir + "/logs", ec_dir);

  if (dict_dir.empty()) {
    dict_dir = FindDefaultDictDir();
  }
  if (db_key.empty()) {
    auto key_res = soeasy::common::MachineKeyProvider::GetOrCreateDatabaseKey(app_data_dir);
    if (key_res.success) {
      db_key = key_res.encryption_key;
    } else {
      db_key = soeasy::common::MachineKeyProvider::kDefaultLegacyKey;
    }
  }

  std::string log_path = app_data_dir + "/logs/soeasy_cli.log";
  if (verbose) {
    soeasy::common::Logger::Init(log_path, 5 * 1024 * 1024, 1, true, spdlog::level::debug);
    std::cerr << "[+] AppData Dir: " << app_data_dir << "\n"
              << "[+] Database:    " << db_path << "\n"
              << "[+] Dict Dir:    " << dict_dir << "\n";
  } else {
    soeasy::common::Logger::Init(log_path, 5 * 1024 * 1024, 1, false, spdlog::level::off);
  }

  soeasy::storage::StorageEngine storage;
  if (!storage.Open(db_path, db_key, dict_dir)) {
    std::cerr << "[-] Failed to open database at: " << db_path << "\n"
              << "    Error: " << storage.GetLastError() << "\n";
    return 1;
  }

  if (command == "search") {
    return HandleSearch(storage, query, limit, offset, path_prefix, json_output, show_snippet);
  } else if (command == "stats") {
    return HandleStats(storage, json_output, verbose);
  } else if (command == "index") {
    return HandleIndex(storage, index_path, verbose);
  } else {
    std::cerr << "[-] Unknown command: " << command << "\n\n";
    PrintUsage(argv[0]);
    return 2;
  }
}
