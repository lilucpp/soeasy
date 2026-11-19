// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>

#include "common/logger.h"
#include "test_helper.h"
#include "worker/l1_text_parser.h"
#include "worker/l2_office_parser.h"
#include "worker/l3_image_parser.h"
#include "worker/parser_registry.h"

namespace {

// Mock specialized parser (e.g. simulating future FEAT-004 Archive Parser)
class MockArchiveParser : public soeasy::worker::IDocumentParser {
 public:
  bool CanHandle(const soeasy::worker::ParseContext &ctx) const override {
    return ctx.ext == "zip" || ctx.ext == "tar" || ctx.ext == "7z";
  }

  bool Parse(const soeasy::worker::ParseContext &ctx, soeasy::worker::ExtractedDocument *out_doc) override {
    if (!out_doc) return false;
    out_doc->utf8_text = "ARCHIVE_MOCK_CONTENT: " + ctx.file_path;
    out_doc->is_scanned_pdf = false;
    out_doc->page_count = 1;
    return true;
  }

  int Priority() const override { return 25; }  // Higher than L2 (20), lower than L3 (30)
  std::string Name() const override { return "MockArchiveParser"; }
};

void TestPrioritySortingAndChainDispatch() {
  std::cout << "[Step 1] 测试责任链按优先级自动排序与任务分发..." << std::endl;

  soeasy::worker::ParserRegistry registry;
  // Intentionally register in non-sorted order: L1 (10), L3 (30), Mock (25), L2 (20)
  registry.RegisterParser(std::make_unique<soeasy::worker::L1TextParser>());
  registry.RegisterParser(std::make_unique<soeasy::worker::L3ImageParser>());
  registry.RegisterParser(std::make_unique<MockArchiveParser>());
  registry.RegisterParser(std::make_unique<soeasy::worker::L2OfficeParser>());

  assert(registry.size() == 4);
  assert(registry.IsSupported("png"));
  assert(registry.IsSupported("docx"));
  assert(registry.IsSupported("zip"));
  assert(registry.IsSupported("cpp"));

  // 1. Dispatch archive file -> should hit MockArchiveParser (Priority 25)
  soeasy::worker::ParseContext zip_ctx;
  zip_ctx.file_path = soeasy::test::GetTestTempPath("test.zip").string();
  zip_ctx.ext = "zip";
  soeasy::worker::ExtractedDocument zip_doc;

  bool ok = registry.ParseDocument(zip_ctx, &zip_doc);
  assert(ok);
  (void)ok;
  assert(zip_doc.utf8_text.find("ARCHIVE_MOCK_CONTENT") != std::string::npos);
  std::cout << "  • MockArchiveParser 命中并解析成功: " << zip_doc.utf8_text << std::endl;

  // 2. Dispatch plain text file -> should fallback to L1TextParser (Priority 10)
  std::string tmp_txt = soeasy::test::GetTestTempPath("soeasy_registry_test.txt").string();
  {
    std::ofstream ofs(tmp_txt);
    ofs << "Hello SoEasy Registry Polymorphism!";
  }

  soeasy::worker::ParseContext txt_ctx;
  txt_ctx.file_path = tmp_txt;
  txt_ctx.ext = "txt";
  soeasy::worker::ExtractedDocument txt_doc;

  ok = registry.ParseDocument(txt_ctx, &txt_doc);
  assert(ok);
  (void)ok;
  assert(txt_doc.utf8_text.find("Hello SoEasy Registry Polymorphism!") != std::string::npos);
  std::cout << "  • L1TextParser 兜底命中并解析成功: " << txt_doc.utf8_text << std::endl;

  std::filesystem::remove(tmp_txt);
  std::cout << "[+] 责任链分发与优先级验证通过!" << std::endl;
}

void TestDefaultRegistryFactory() {
  std::cout << "[Step 2] 测试默认生产环境 ParserRegistry 工厂函数..." << std::endl;

  auto registry = soeasy::worker::ParserRegistry::CreateDefaultRegistry();
  assert(registry != nullptr);
  assert(registry->size() == 3);

  // Office format
  assert(registry->IsSupported("docx"));
  assert(registry->IsSupported("pdf"));
  assert(registry->IsSupported("xlsx"));

  // Image format
  assert(registry->IsSupported("png"));
  assert(registry->IsSupported("jpg"));

  // Text format
  assert(registry->IsSupported("cpp"));
  assert(registry->IsSupported("txt"));

  std::cout << "[+] DefaultRegistry 工厂与支持格式嗅探验证通过!" << std::endl;
}

void TestParseStream() {
  std::cout << "[Step 3] 测试 IDocumentParser & L1TextParser 统一流式解析 (ParseStream)..." << std::endl;

  soeasy::worker::L1TextParser parser;
  soeasy::worker::ParseContext ctx;
  ctx.doc_id = 999;
  ctx.ext = "txt";

  // 1. English text stream
  std::istringstream iss1("Hello SoEasy Unix Stream Parser!\nLine 2 for pipeline testing.");
  soeasy::worker::ExtractedDocument doc1;
  bool ok1 = parser.ParseStream(iss1, ctx, &doc1);
  assert(ok1);
  assert(doc1.utf8_text.find("Unix Stream Parser") != std::string::npos);
  assert(doc1.utf8_text.find("Line 2 for pipeline testing.") != std::string::npos);

  // 2. Chinese UTF-8 stream
  std::istringstream iss2("基于 Unix 管道的流式文本解析器，无需中间临时落盘文件。");
  soeasy::worker::ExtractedDocument doc2;
  bool ok2 = parser.ParseStream(iss2, ctx, &doc2);
  assert(ok2);
  assert(doc2.utf8_text.find("基于 Unix 管道的流式文本解析器") != std::string::npos);

  // 3. Empty stream
  std::istringstream iss3("");
  soeasy::worker::ExtractedDocument doc3;
  bool ok3 = parser.ParseStream(iss3, ctx, &doc3);
  assert(ok3);
  assert(doc3.utf8_text.empty());

  // 4. Default base class implementation
  MockArchiveParser mock_parser;
  std::istringstream iss4("dummy archive data");
  soeasy::worker::ExtractedDocument doc4;
  bool ok4 = mock_parser.ParseStream(iss4, ctx, &doc4);
  assert(!ok4);  // Base default returns false

  std::cout << "[+] ParseStream 流式解析验证通过!" << std::endl;
}

}  // namespace

int main() {
  soeasy::common::Logger::Init(soeasy::test::GetTestTempPath("test_parser_registry.log").string(), 10 * 1024 * 1024, 1,
                               false);
  std::cout << "========================================================" << std::endl;
  std::cout << "   SoEasy Worker 解析器策略责任链单元测试 (Stage 1)     " << std::endl;
  std::cout << "========================================================" << std::endl;

  TestPrioritySortingAndChainDispatch();
  TestDefaultRegistryFactory();
  TestParseStream();

  std::cout << "\n🎉 所有验证项全部通过! ParserRegistry 架构重构验证成功!" << std::endl;
  return 0;
}
