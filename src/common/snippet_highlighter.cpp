// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include "common/snippet_highlighter.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>
#include <utility>

#include "utf8.h"

namespace soeasy {
namespace common {

std::vector<std::string> SnippetHighlighter::ExtractKeywordsFromQuery(const std::string &query) {
  std::vector<std::string> kws;
  std::istringstream iss(query);
  std::string token;

  auto IsBooleanOp = [](const std::string &s) {
    if (s.size() == 2) {
      return (s[0] == 'O' || s[0] == 'o') && (s[1] == 'R' || s[1] == 'r');
    }
    if (s.size() == 3) {
      bool is_and = (s[0] == 'A' || s[0] == 'a') && (s[1] == 'N' || s[1] == 'n') && (s[2] == 'D' || s[2] == 'd');
      bool is_not = (s[0] == 'N' || s[0] == 'n') && (s[1] == 'O' || s[1] == 'o') && (s[2] == 'T' || s[2] == 't');
      return is_and || is_not;
    }
    return false;
  };

  auto IsNumeric = [](const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) {
      if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
  };

  while (iss >> token) {
    // 1. Handle NEAR(...) syntax prefix (extracting contained search terms)
    if (token.rfind("NEAR(", 0) == 0 || token.rfind("near(", 0) == 0) {
      token = token.substr(5);
    }

    // 2. Strip column prefix e.g. "body:value" -> "value", "title:abc" -> "abc"
    size_t colon = token.find(':');
    if (colon != std::string::npos) {
      token = token.substr(colon + 1);
    }

    // 3. Trim leading punctuation: quotes, brackets, colons, commas
    while (!token.empty()) {
      char c = token.front();
      if (c == '"' || c == '\'' || c == '(' || c == '[' || c == '{' || c == ':' || c == ',') {
        token.erase(token.begin());
      } else {
        break;
      }
    }

    // 4. Trim trailing punctuation: quotes, brackets, asterisks, commas, semicolons, colons
    while (!token.empty()) {
      char c = token.back();
      if (c == '"' || c == '\'' || c == ')' || c == ']' || c == '}' || c == '*' || c == ',' || c == ';' || c == ':') {
        token.pop_back();
      } else {
        break;
      }
    }

    if (token.empty()) continue;

    // 5. Skip boolean operators (AND, OR, NOT, case-insensitive)
    if (IsBooleanOp(token)) continue;

    // 6. Skip pure digits (often NEAR distance parameter e.g. NEAR(a b, 10))
    if (IsNumeric(token)) continue;

    // 7. Deduplicate while preserving first-seen order
    if (std::find(kws.begin(), kws.end(), token) == kws.end()) {
      kws.push_back(token);
    }
  }

  return kws;
}

std::string SnippetHighlighter::GenerateSnippet(const std::string &full_text,
                                                const std::vector<std::string> &keywords,
                                                size_t max_chars) {
  if (full_text.empty() || keywords.empty()) {
    return "";
  }

  // Lowercase version of full_text for case-insensitive search
  std::string lower_text = full_text;
  for (char &c : lower_text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + 32);
    }
  }

  // Find the earliest matching keyword position (prefer longer keyword when positions tie)
  size_t best_pos = std::string::npos;
  size_t best_kw_len = 0;
  for (const auto &kw : keywords) {
    if (kw.empty()) continue;
    std::string lower_kw = kw;
    for (char &c : lower_kw) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    size_t pos = lower_text.find(lower_kw);
    if (pos != std::string::npos) {
      if (best_pos == std::string::npos || pos < best_pos || (pos == best_pos && kw.size() > best_kw_len)) {
        best_pos = pos;
        best_kw_len = kw.size();
      }
    }
  }

  // Calculate snippet window surrounding the match
  size_t start_pos = 0;
  size_t end_pos = 0;
  if (best_pos != std::string::npos) {
    size_t context_padding = max_chars / 3;
    if (best_pos > context_padding) {
      start_pos = best_pos - context_padding;
    } else {
      start_pos = 0;
    }
    // Ensure window is at least large enough to encompass the matched keyword
    size_t window_len = std::max(max_chars, best_kw_len);
    end_pos = std::min(full_text.size(), start_pos + window_len);

    // Adjust start_pos to valid UTF-8 character boundary
    while (start_pos > 0 && (static_cast<unsigned char>(full_text[start_pos]) & 0xC0) == 0x80) {
      --start_pos;
    }
    // Adjust end_pos to valid UTF-8 character boundary
    while (end_pos < full_text.size() && (static_cast<unsigned char>(full_text[end_pos]) & 0xC0) == 0x80) {
      ++end_pos;
    }
  } else {
    // No keyword found; take prefix of text
    end_pos = std::min(full_text.size(), max_chars);
    while (end_pos < full_text.size() && (static_cast<unsigned char>(full_text[end_pos]) & 0xC0) == 0x80) {
      ++end_pos;
    }
  }

  std::string raw_snippet = full_text.substr(start_pos, end_pos - start_pos);

  // Validate UTF-8 encoding
  if (!utf8::is_valid(raw_snippet.begin(), raw_snippet.end())) {
    std::string cleaned;
    utf8::replace_invalid(raw_snippet.begin(), raw_snippet.end(), std::back_inserter(cleaned));
    raw_snippet = std::move(cleaned);
  }

  std::string lower_snippet = raw_snippet;
  for (char &c : lower_snippet) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }

  struct Span {
    size_t start;
    size_t end;
  };
  std::vector<Span> spans;

  // Sort keywords by length descending to process longer phrases first
  std::vector<std::string> sorted_keywords = keywords;
  std::sort(sorted_keywords.begin(), sorted_keywords.end(),
            [](const std::string &a, const std::string &b) { return a.size() > b.size(); });

  for (const auto &kw : sorted_keywords) {
    if (kw.empty()) continue;
    std::string lower_kw = kw;
    for (char &c : lower_kw) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }

    size_t search_start = 0;
    while ((search_start = lower_snippet.find(lower_kw, search_start)) != std::string::npos) {
      spans.push_back({search_start, search_start + lower_kw.size()});
      search_start += lower_kw.size();
    }
  }

  // Sort spans by start position ascending
  std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) {
    if (a.start != b.start) return a.start < b.start;
    return a.end > b.end;
  });

  // Merge overlapping or adjacent spans
  std::vector<Span> merged;
  for (const auto &s : spans) {
    if (merged.empty()) {
      merged.push_back(s);
    } else {
      if (s.start <= merged.back().end) {
        merged.back().end = std::max(merged.back().end, s.end);
      } else {
        merged.push_back(s);
      }
    }
  }

  auto EscapeHtml = [](const std::string &str) -> std::string {
    std::string out;
    out.reserve(str.size() + 16);
    for (char c : str) {
      switch (c) {
        case '&':
          out.append("&amp;");
          break;
        case '<':
          out.append("&lt;");
          break;
        case '>':
          out.append("&gt;");
          break;
        case '"':
          out.append("&quot;");
          break;
        default:
          out.push_back(c);
          break;
      }
    }
    return out;
  };

  std::string html;
  html.reserve(raw_snippet.size() * 2);
  size_t cursor = 0;

  for (const auto &span : merged) {
    if (span.start > cursor) {
      html.append(EscapeHtml(raw_snippet.substr(cursor, span.start - cursor)));
    }
    html.append("<mark>");
    html.append(EscapeHtml(raw_snippet.substr(span.start, span.end - span.start)));
    html.append("</mark>");
    cursor = span.end;
  }

  if (cursor < raw_snippet.size()) {
    html.append(EscapeHtml(raw_snippet.substr(cursor)));
  }

  if (start_pos > 0) {
    html = "..." + html;
  }
  if (end_pos < full_text.size()) {
    html.append("...");
  }

  return html;
}

}  // namespace common
}  // namespace soeasy
