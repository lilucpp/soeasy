// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/snippet_highlighter.h"
#include "utf8.h"

namespace soeasy {
namespace test {

using common::SnippetHighlighter;

// -----------------------------------------------------------------------------
// AAA Pattern Tests: ExtractKeywordsFromQuery
// -----------------------------------------------------------------------------

TEST(SnippetHighlighterTest, ExtractKeywordsFromQueryEmptyAndWhitespace) {
  // 1. Arrange & Act
  auto res1 = SnippetHighlighter::ExtractKeywordsFromQuery("");
  auto res2 = SnippetHighlighter::ExtractKeywordsFromQuery("   \t\r\n  ");

  // 3. Assert
  EXPECT_TRUE(res1.empty());
  EXPECT_TRUE(res2.empty());
}

TEST(SnippetHighlighterTest, ExtractKeywordsFromQueryFiltersBooleanOperatorsCaseInsensitively) {
  // 1. Arrange
  const std::string query = "apple AND banana or orange NOT pear not peach";

  // 2. Act
  auto kws = SnippetHighlighter::ExtractKeywordsFromQuery(query);

  // 3. Assert: Boolean keywords must be excluded
  ASSERT_EQ(kws.size(), 5u);
  EXPECT_EQ(kws[0], "apple");
  EXPECT_EQ(kws[1], "banana");
  EXPECT_EQ(kws[2], "orange");
  EXPECT_EQ(kws[3], "pear");
  EXPECT_EQ(kws[4], "peach");
}

TEST(SnippetHighlighterTest, ExtractKeywordsFromQueryHandlesNearSyntax) {
  // 1. Arrange: FTS5 NEAR(term1 term2, 10)
  const std::string query = "NEAR(distributed database, 10)";

  // 2. Act
  auto kws = SnippetHighlighter::ExtractKeywordsFromQuery(query);

  // 3. Assert: Must extract the actual search terms and exclude distance parameter
  ASSERT_EQ(kws.size(), 2u);
  EXPECT_EQ(kws[0], "distributed");
  EXPECT_EQ(kws[1], "database");
}

TEST(SnippetHighlighterTest, ExtractKeywordsFromQueryFieldFiltersAndPunctuation) {
  // 1. Arrange: Column filters, quotes, asterisks, brackets
  const std::string query = "body:database file_name:\"main.cpp\" 'important' (keyword) prefix*";

  // 2. Act
  auto kws = SnippetHighlighter::ExtractKeywordsFromQuery(query);

  // 3. Assert
  ASSERT_EQ(kws.size(), 5u);
  EXPECT_EQ(kws[0], "database");
  EXPECT_EQ(kws[1], "main.cpp");
  EXPECT_EQ(kws[2], "important");
  EXPECT_EQ(kws[3], "keyword");
  EXPECT_EQ(kws[4], "prefix");
}

TEST(SnippetHighlighterTest, ExtractKeywordsFromQueryPathologicalInputs) {
  // 1. Arrange: Empty quotes, empty brackets, dangling colon
  const std::string query = "\"\" '' () * body: : :::: ,,,, ;;;;";

  // 2. Act
  auto kws = SnippetHighlighter::ExtractKeywordsFromQuery(query);

  // 3. Assert: All pure punctuation or dangling prefix must be trimmed to empty
  EXPECT_TRUE(kws.empty());
}

TEST(SnippetHighlighterTest, ExtractKeywordsFromQueryUnicodeAndDeduplication) {
  // 1. Arrange: Chinese terms with duplicates
  const std::string query = "全文检索 倒排索引 全文检索 C++20 倒排索引";

  // 2. Act
  auto kws = SnippetHighlighter::ExtractKeywordsFromQuery(query);

  // 3. Assert: Distinct list in first-seen order
  ASSERT_EQ(kws.size(), 3u);
  EXPECT_EQ(kws[0], "全文检索");
  EXPECT_EQ(kws[1], "倒排索引");
  EXPECT_EQ(kws[2], "C++20");
}

// -----------------------------------------------------------------------------
// AAA Pattern Tests: GenerateSnippet
// -----------------------------------------------------------------------------

TEST(SnippetHighlighterTest, GenerateSnippetEmptyInputs) {
  EXPECT_EQ(SnippetHighlighter::GenerateSnippet("", {"test"}, 100), "");
  EXPECT_EQ(SnippetHighlighter::GenerateSnippet("Some content", {}, 100), "");
  EXPECT_EQ(SnippetHighlighter::GenerateSnippet("", {}, 100), "");
}

TEST(SnippetHighlighterTest, GenerateSnippetKeywordNotFoundReturnsPrefix) {
  const std::string text = "Short text here without query";
  auto snippet = SnippetHighlighter::GenerateSnippet(text, {"missing"}, 100);
  EXPECT_EQ(snippet, text);
}

TEST(SnippetHighlighterTest, GenerateSnippetCaseInsensitiveMatching) {
  // 1. Arrange
  const std::string text = "The Quick Brown FOX jumps over the lazy DOG";

  // 2. Act
  auto snippet = SnippetHighlighter::GenerateSnippet(text, {"fox", "dog", "the"}, 100);

  // 3. Assert: Matches case-insensitively but preserves original casing inside <mark>
  EXPECT_NE(snippet.find("<mark>The</mark>"), std::string::npos);
  EXPECT_NE(snippet.find("<mark>FOX</mark>"), std::string::npos);
  EXPECT_NE(snippet.find("<mark>the</mark>"), std::string::npos);
  EXPECT_NE(snippet.find("<mark>DOG</mark>"), std::string::npos);
}

TEST(SnippetHighlighterTest, GenerateSnippetOverlappingAndAdjacentKeywordsMerged) {
  // 1. Adjacent keywords: "foo" + "bar" in "foobar" -> <mark>foobar</mark>
  auto snip_adj = SnippetHighlighter::GenerateSnippet("foobar", {"foo", "bar"}, 100);
  EXPECT_EQ(snip_adj, "<mark>foobar</mark>");

  // 2. Overlapping keywords: "an" + "nan" in "banana" -> b<mark>anan</mark>a
  auto snip_overlap = SnippetHighlighter::GenerateSnippet("banana", {"an", "nan"}, 100);
  EXPECT_EQ(snip_overlap, "b<mark>anan</mark>a");
}

TEST(SnippetHighlighterTest, GenerateSnippetHtmlEscapingPreventsXss) {
  // 1. Arrange: Malicious input with script tags, ampersand, and quotes
  const std::string evil = "<script>alert('xss & \"hack\"');</script>";

  // 2. Act
  auto snippet = SnippetHighlighter::GenerateSnippet(evil, {"alert"}, 100);

  // 3. Assert: Must escape HTML special chars while cleanly wrapping <mark>
  EXPECT_EQ(snippet.find("<script>"), std::string::npos);
  EXPECT_NE(snippet.find("&lt;script&gt;"), std::string::npos);
  EXPECT_NE(snippet.find("<mark>alert</mark>"), std::string::npos);
  EXPECT_NE(snippet.find("&amp;"), std::string::npos);
  EXPECT_NE(snippet.find("&quot;"), std::string::npos);
  EXPECT_NE(snippet.find("&lt;/script&gt;"), std::string::npos);
}

TEST(SnippetHighlighterTest, GenerateSnippetUtf8BoundaryIntegrityInChineseText) {
  // 1. Arrange: Long Chinese text where 30 chars falls into multi-byte characters
  const std::string cjk_text =
      "这是一段非常非常长的中文测试示例文本文档，用于验证切片滑动窗口在截断UTF-8多字节字符时绝不会截断到字符中间导致乱码或乱字符。";

  // 2. Act
  auto snippet = SnippetHighlighter::GenerateSnippet(cjk_text, {"滑动窗口"}, 30);

  // 3. Assert: Output must be strictly valid UTF-8 and contain properly highlighted keyword
  EXPECT_TRUE(utf8::is_valid(snippet.begin(), snippet.end()));
  EXPECT_NE(snippet.find("<mark>滑动窗口</mark>"), std::string::npos);
}

TEST(SnippetHighlighterTest, GenerateSnippetSmallWindowEncompassesKeyword) {
  // 1. Arrange: max_chars=3 is smaller than keyword "Hello" (length 5)
  const std::string text = "Hello World";

  // 2. Act
  auto snippet = SnippetHighlighter::GenerateSnippet(text, {"Hello"}, 3);

  // 3. Assert: Snippet window must expand to cleanly encompass and highlight "Hello"
  EXPECT_NE(snippet.find("<mark>Hello</mark>"), std::string::npos);
}

TEST(SnippetHighlighterTest, GenerateSnippetMiddleMatchHasEllipsesOnBothSides) {
  // 1. Arrange: Long head and tail
  const std::string head(50, 'A');
  const std::string tail(50, 'B');
  const std::string text = head + "_TARGET_" + tail;

  // 2. Act
  auto snippet = SnippetHighlighter::GenerateSnippet(text, {"TARGET"}, 30);

  // 3. Assert: Must start and end with ellipsis
  EXPECT_EQ(snippet.rfind("...", 0), 0u);
  ASSERT_GE(snippet.size(), 3u);
  EXPECT_EQ(snippet.substr(snippet.size() - 3), "...");
  EXPECT_NE(snippet.find("<mark>TARGET</mark>"), std::string::npos);
}

TEST(SnippetHighlighterTest, GenerateSnippetLargeDocumentBoundedScanDefense) {
  // 1. Arrange: Create a 200KB large text (>128KB scan window)
  std::string large_text;
  large_text.reserve(200 * 1024);
  for (int i = 0; i < 2000; ++i) {
    large_text.append("段落内容填充行，用于测试系统内存占用与超大文档安全截断防御。");
  }
  // Insert target keyword within the first 128KB (e.g. at ~10KB)
  const std::string keyword = "关键安全标记";
  large_text.insert(10000, keyword);

  // 2. Act
  auto snippet = SnippetHighlighter::GenerateSnippet(large_text, {keyword}, 60);

  // 3. Assert
  EXPECT_TRUE(utf8::is_valid(snippet.begin(), snippet.end()));
  EXPECT_NE(snippet.find("<mark>" + keyword + "</mark>"), std::string::npos);
  EXPECT_LT(snippet.size(), 300u);
}

TEST(SnippetHighlighterTest, GenerateSnippetLargeDocumentBeyondWindowGracefulPrefix) {
  // 1. Arrange: 150KB large text where keyword only appears past 140KB (>128KB window)
  std::string large_text(150 * 1024, 'X');
  const std::string keyword = "LATE_KEYWORD";
  large_text.replace(145 * 1024, keyword.size(), keyword);

  // 2. Act
  auto snippet = SnippetHighlighter::GenerateSnippet(large_text, {keyword}, 40);

  // 3. Assert: Beyond scan window, safely falls back to text prefix without crashing or memory bloat
  EXPECT_FALSE(snippet.empty());
  EXPECT_TRUE(utf8::is_valid(snippet.begin(), snippet.end()));
  EXPECT_EQ(snippet.find("<mark>"), std::string::npos);
  EXPECT_EQ(snippet.substr(snippet.size() - 3), "...");
}

}  // namespace test
}  // namespace soeasy
