// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <thread>
#include <vector>

#include "common/snippet_highlighter.h"
#include "miniz.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"
#include "tests/test_helper.h"
#include "worker/l2_office_parser.h"

#define CHECK(expr)                                                                               \
  do {                                                                                            \
    if (!(expr)) {                                                                                \
      std::cerr << "[-] CHECK FAILED: " #expr " at " << __FILE__ << ":" << __LINE__ << std::endl; \
      std::cout << "[-] CHECK FAILED: " #expr " at " << __FILE__ << ":" << __LINE__ << std::endl; \
      std::fflush(stdout);                                                                        \
      std::fflush(stderr);                                                                        \
      std::abort();                                                                               \
    }                                                                                             \
  } while (0)

namespace {

uint64_t GetFileSize(const std::string &path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    return static_cast<uint64_t>(st.st_size);
  }
  return 0;
}

void CreateSampleDocx(const std::string &path) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  CHECK(mz_zip_writer_init_file(&zip, path.c_str(), 0));

  const std::string content_types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
      "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
      "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
      "  <Override PartName=\"/word/document.xml\""
      " ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>\n"
      "</Types>";
  mz_zip_writer_add_mem(&zip, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_BEST_COMPRESSION);

  const std::string root_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>\n"
      "</Relationships>";
  mz_zip_writer_add_mem(&zip, "_rels/.rels", root_rels.data(), root_rels.size(), MZ_BEST_COMPRESSION);

  const std::string document_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\n"
      "  <w:body>\n"
      "    <w:p>\n"
      "      <w:r><w:t>人工智能战略规划与核心算法研发白皮书</w:t></w:r>\n"
      "    </w:p>\n"
      "    <w:p>\n"
      "      <w:r><w:t>SoEasy 团队于 2026 年正式启动 Office 深度解析流水线。</w:t></w:r>\n"
      "    </w:p>\n"
      "    <w:p>\n"
      "      <w:r><w:t>本文档包含 XML 转义实体：&lt;核心技术&gt; &amp; &quot;卓越创新&quot;。</w:t></w:r>\n"
      "    </w:p>\n"
      "  </w:body>\n"
      "</w:document>";
  mz_zip_writer_add_mem(&zip, "word/document.xml", document_xml.data(), document_xml.size(), MZ_BEST_COMPRESSION);

  CHECK(mz_zip_writer_finalize_archive(&zip));
  CHECK(mz_zip_writer_end(&zip));
}

void CreateSampleXlsx(const std::string &path) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  CHECK(mz_zip_writer_init_file(&zip, path.c_str(), 0));

  const std::string content_types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
      "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
      "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
      "  <Override PartName=\"/xl/workbook.xml\""
      " ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>\n"
      "  <Override PartName=\"/xl/worksheets/sheet1.xml\""
      " ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>\n"
      "  <Override PartName=\"/xl/sharedStrings.xml\""
      " ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>\n"
      "</Types>";
  mz_zip_writer_add_mem(&zip, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_BEST_COMPRESSION);

  const std::string root_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>\n"
      "</Relationships>";
  mz_zip_writer_add_mem(&zip, "_rels/.rels", root_rels.data(), root_rels.size(), MZ_BEST_COMPRESSION);

  const std::string workbook_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\""
      " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">\n"
      "  <sheets>\n"
      "    <sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/>\n"
      "  </sheets>\n"
      "</workbook>";
  mz_zip_writer_add_mem(&zip, "xl/workbook.xml", workbook_xml.data(), workbook_xml.size(), MZ_BEST_COMPRESSION);

  const std::string workbook_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>\n"
      "  <Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>\n"
      "</Relationships>";
  mz_zip_writer_add_mem(&zip, "xl/_rels/workbook.xml.rels", workbook_rels.data(), workbook_rels.size(),
                        MZ_BEST_COMPRESSION);

  const std::string shared_strings =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"3\" uniqueCount=\"3\">\n"
      "  <si><t>2026年度第一季度财务报表与研发开支明细</t></si>\n"
      "  <si><t>服务器集群购置费用</t></si>\n"
      "  <si><t>算法工程师与架构师薪酬开支</t></si>\n"
      "</sst>";
  mz_zip_writer_add_mem(&zip, "xl/sharedStrings.xml", shared_strings.data(), shared_strings.size(),
                        MZ_BEST_COMPRESSION);

  const std::string sheet1 =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n"
      "  <sheetData>\n"
      "    <row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c></row>\n"
      "    <row r=\"2\"><c r=\"A2\" t=\"s\"><v>1</v></c><c r=\"B2\"><v>850000</v></c></row>\n"
      "  </sheetData>\n"
      "</worksheet>";
  mz_zip_writer_add_mem(&zip, "xl/worksheets/sheet1.xml", sheet1.data(), sheet1.size(), MZ_BEST_COMPRESSION);

  CHECK(mz_zip_writer_finalize_archive(&zip));
  CHECK(mz_zip_writer_end(&zip));
}

void CreateSamplePptx(const std::string &path) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  CHECK(mz_zip_writer_init_file(&zip, path.c_str(), 0));

  const std::string content_types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
      "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
      "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
      "  <Override PartName=\"/ppt/presentation.xml\""
      " ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>\n"
      "  <Override PartName=\"/ppt/slides/slide1.xml\""
      " ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>\n"
      "</Types>";
  mz_zip_writer_add_mem(&zip, "[Content_Types].xml", content_types.data(), content_types.size(), MZ_BEST_COMPRESSION);

  const std::string root_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/>\n"
      "</Relationships>";
  mz_zip_writer_add_mem(&zip, "_rels/.rels", root_rels.data(), root_rels.size(), MZ_BEST_COMPRESSION);

  const std::string presentation_xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<p:presentation xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
      " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
      " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">\n"
      "  <p:sldIdLst>\n"
      "    <p:sldId id=\"256\" r:id=\"rId1\"/>\n"
      "  </p:sldIdLst>\n"
      "</p:presentation>";
  mz_zip_writer_add_mem(&zip, "ppt/presentation.xml", presentation_xml.data(), presentation_xml.size(),
                        MZ_BEST_COMPRESSION);

  const std::string presentation_rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
      "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide1.xml\"/>\n"
      "</Relationships>";
  mz_zip_writer_add_mem(&zip, "ppt/_rels/presentation.xml.rels", presentation_rels.data(), presentation_rels.size(),
                        MZ_BEST_COMPRESSION);

  const std::string slide1 =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
      "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">\n"
      "  <p:cSld>\n"
      "    <p:spTree>\n"
      "      <p:sp>\n"
      "        <p:txBody>\n"
      "          <a:p><a:r><a:t>SoEasy 桌面级全文检索系统路演发布会</a:t></a:r></a:p>\n"
      "          <a:p><a:r><a:t>汇报人：系统架构设计总监</a:t></a:r></a:p>\n"
      "        </p:txBody>\n"
      "      </p:sp>\n"
      "    </p:spTree>\n"
      "  </p:cSld>\n"
      "</p:sld>";
  mz_zip_writer_add_mem(&zip, "ppt/slides/slide1.xml", slide1.data(), slide1.size(), MZ_BEST_COMPRESSION);

  CHECK(mz_zip_writer_finalize_archive(&zip));
  CHECK(mz_zip_writer_end(&zip));
}

void CreateSamplePdf(const std::string &path) {
  // Generate a strictly standard-compliant PDF 1.4 document with exact byte offsets
  std::ofstream ofs(path, std::ios::binary);
  std::string pdf_data =
      "%PDF-1.4\n"
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
      "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
      "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n"
      "4 0 obj\n<< /Length 71 >>\nstream\n"
      "BT\n/F1 18 Tf\n100 700 Td\n(SoEasy Deep PDF Document Search Engine) Tj\nET\nendstream\nendobj\n"
      "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n";
  size_t off1 = pdf_data.find("1 0 obj");
  size_t off2 = pdf_data.find("2 0 obj");
  size_t off3 = pdf_data.find("3 0 obj");
  size_t off4 = pdf_data.find("4 0 obj");
  size_t off5 = pdf_data.find("5 0 obj");
  size_t xref_off = pdf_data.size();

  auto format_xref_entry = [](size_t offset, bool in_use) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010lu %05u %c \n", static_cast<unsigned long>(offset), in_use ? 0 : 65535,
                  in_use ? 'n' : 'f');
    return std::string(buf);
  };

  std::string xref = "xref\n0 6\n";
  xref += format_xref_entry(0, false);
  xref += format_xref_entry(off1, true);
  xref += format_xref_entry(off2, true);
  xref += format_xref_entry(off3, true);
  xref += format_xref_entry(off4, true);
  xref += format_xref_entry(off5, true);
  xref += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref_off) + "\n%%EOF\n";

  pdf_data.append(xref);
  ofs.write(pdf_data.data(), pdf_data.size());
}

void CreateScannedSamplePdf(const std::string &path) {
  // Generate a standard-compliant PDF containing image raster bytes with valid xref
  std::ofstream ofs(path, std::ios::binary);
  std::string dummy_pixels(20000, 'x');
  std::string pdf_data =
      "%PDF-1.4\n"
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
      "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
      "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>\nendobj\n"
      "4 0 obj\n<< /Length 20000 >>\nstream\n" + dummy_pixels + "\nendstream\nendobj\n";
  size_t off1 = pdf_data.find("1 0 obj");
  size_t off2 = pdf_data.find("2 0 obj");
  size_t off3 = pdf_data.find("3 0 obj");
  size_t off4 = pdf_data.find("4 0 obj");
  size_t xref_off = pdf_data.size();

  auto format_xref_entry = [](size_t offset, bool in_use) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%010lu %05u %c \n", static_cast<unsigned long>(offset), in_use ? 0 : 65535,
                  in_use ? 'n' : 'f');
    return std::string(buf);
  };

  std::string xref = "xref\n0 5\n";
  xref += format_xref_entry(0, false);
  xref += format_xref_entry(off1, true);
  xref += format_xref_entry(off2, true);
  xref += format_xref_entry(off3, true);
  xref += format_xref_entry(off4, true);
  xref += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + std::to_string(xref_off) + "\n%%EOF\n";

  pdf_data.append(xref);
  ofs.write(pdf_data.data(), pdf_data.size());
}

}  // namespace

int main(int argc, char *argv[]) {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy L2 办公文档与富文本深度解析实证测试 (Phase 5)\n";
  std::cout << "   DocToText (lithiumFlower) + OOXML + PDF + 扫描件识别 + 沙箱与检索\n";
  std::cout << "========================================================\n\n";

  const std::string test_dir = soeasy::test::GetTestTempPath("soeasy_phase5_test").string();
  const std::string test_db = soeasy::test::GetTestTempPath("soeasy_phase5_test.db").string();
  const std::string passphrase = "SoEasy_Phase5_Passphrase_2026!";
  const std::string dict_dir = soeasy::test::FindDictDir();

  std::filesystem::remove_all(test_dir);
  std::filesystem::create_directories(test_dir);
  std::remove(test_db.c_str());
  std::remove((test_db + "-wal").c_str());
  std::remove((test_db + "-shm").c_str());

  soeasy::worker::L2OfficeParser parser;

  // -------------------------------------------------------------
  // Test 1: DOCX Parsing & XML Unescaping
  // -------------------------------------------------------------
  std::cout << "[Step 1] 测试 Word 文档 (.docx) 结构解压与正文抽取...\n";
  std::string docx_path = test_dir + "/sample.docx";
  CreateSampleDocx(docx_path);

  std::string docx_text;
  bool docx_ok = parser.Parse(docx_path, "docx", &docx_text);
  CHECK(docx_ok);
  CHECK(docx_text.find("人工智能战略规划") != std::string::npos);
  CHECK(docx_text.find("2026 年正式启动") != std::string::npos);
  std::cout << "  [+] DOCX 提取成功! 文本长度: " << docx_text.size() << " 字符\n"
            << "      预览: " << docx_text.substr(0, 50) << "...\n";

  // -------------------------------------------------------------
  // Test 2: XLSX Parsing & SharedStrings
  // -------------------------------------------------------------
  std::cout << "\n[Step 2] 测试 Excel 电子表格 (.xlsx) 共享字符池与单元格抽取...\n";
  std::string xlsx_path = test_dir + "/sample.xlsx";
  CreateSampleXlsx(xlsx_path);

  std::string xlsx_text;
  bool xlsx_ok = parser.Parse(xlsx_path, "xlsx", &xlsx_text);
  CHECK(xlsx_ok);
  CHECK(xlsx_text.find("2026年度第一季度财务报表") != std::string::npos);
  CHECK(xlsx_text.find("服务器集群购置费用") != std::string::npos);
  std::cout << "  [+] XLSX 提取成功! 文本长度: " << xlsx_text.size() << " 字符\n"
            << "      预览: " << xlsx_text.substr(0, 50) << "...\n";

  // -------------------------------------------------------------
  // Test 3: PPTX Parsing & Slide Deck Extraction
  // -------------------------------------------------------------
  std::cout << "\n[Step 3] 测试 PowerPoint 幻灯片 (.pptx) 多幻灯片文本抽取...\n";
  std::string pptx_path = test_dir + "/sample.pptx";
  CreateSamplePptx(pptx_path);

  std::string pptx_text;
  bool pptx_ok = parser.Parse(pptx_path, "pptx", &pptx_text);
  CHECK(pptx_ok);
  CHECK(pptx_text.find("SoEasy 桌面级全文检索系统路演发布会") != std::string::npos);
  CHECK(pptx_text.find("系统架构设计总监") != std::string::npos);
  std::cout << "  [+] PPTX 提取成功! 文本长度: " << pptx_text.size() << " 字符\n";

  // -------------------------------------------------------------
  // Test 4: PDF Parsing (Digital PDF with text stream)
  // -------------------------------------------------------------
  std::cout << "\n[Step 4] 测试 PDF 文档 (.pdf) 文本层抽取与 Poppler 联动...\n";
  std::string pdf_path = test_dir + "/sample.pdf";
  CreateSamplePdf(pdf_path);

  std::string pdf_text;
  bool is_scanned = false;
  bool pdf_ok = parser.Parse(pdf_path, "pdf", &pdf_text, &is_scanned);
  CHECK(pdf_ok);
  CHECK(!is_scanned);
  CHECK(pdf_text.find("SoEasy Deep PDF Document") != std::string::npos);
  std::cout << "  [+] PDF 提取成功! 提取文本: \"" << pdf_text.substr(0, 40)
            << "\", 扫描件标记: " << (is_scanned ? "是" : "否") << "\n";

  // -------------------------------------------------------------
  // Test 5: Scanned / Image-only PDF Detection
  // -------------------------------------------------------------
  std::cout << "\n[Step 5] 测试 纯图片/扫描件 PDF 自动识别与标记...\n";
  std::string scanned_pdf_path = test_dir + "/scanned.pdf";
  CreateScannedSamplePdf(scanned_pdf_path);

  std::string scanned_text;
  bool is_really_scanned = false;
  parser.Parse(scanned_pdf_path, "pdf", &scanned_text, &is_really_scanned);
  CHECK(is_really_scanned == true);
  std::cout << "  [+] 扫描件精准识别生效! 扫描件标记: 是 (将自动转入 L4 OCR 排队)\n";

  // -------------------------------------------------------------
  // Test 6: Multi-process Sandbox Worker IPC with Office Files
  // -------------------------------------------------------------
  std::cout << "\n[Step 6] 验证多进程沙箱 Worker 跨进程调度解析 Office 文档...\n";
  auto worker_guard = std::make_shared<soeasy::service::WorkerGuard>();
  std::string worker_bin = soeasy::test::FindWorkerBin();
  CHECK(worker_guard->Init(worker_bin, 2, 8000));

  soeasy::service::ParseTask docx_task;
  docx_task.doc_id = 1001;
  docx_task.file_path = docx_path;
  docx_task.ext = "docx";
  docx_task.parse_level = 2;

  soeasy::service::ParseTaskResult docx_task_res;
  bool worker_docx_ok = worker_guard->ExecuteTask(docx_task, &docx_task_res);
  CHECK(worker_docx_ok);
  CHECK(docx_task_res.status == soeasy::service::TaskStatus::kSuccess);

  std::string docx_shm_text = docx_task_res.extracted_text;
  CHECK(docx_shm_text.find("人工智能战略规划") != std::string::npos);
  std::cout << "  [+] 沙箱跨进程 DOCX 解析与 RPC 传输成功! 耗时: " << docx_task_res.elapsed_ms << " ms\n";

  soeasy::service::ParseTask xlsx_task;
  xlsx_task.doc_id = 1002;
  xlsx_task.file_path = xlsx_path;
  xlsx_task.ext = "xlsx";
  xlsx_task.parse_level = 2;

  soeasy::service::ParseTaskResult xlsx_task_res;
  bool worker_xlsx_ok = worker_guard->ExecuteTask(xlsx_task, &xlsx_task_res);
  CHECK(worker_xlsx_ok);
  CHECK(xlsx_task_res.status == soeasy::service::TaskStatus::kSuccess);

  std::string xlsx_shm_text = xlsx_task_res.extracted_text;
  CHECK(xlsx_shm_text.find("2026年度第一季度财务报表") != std::string::npos);
  std::cout << "  [+] 沙箱跨进程 XLSX 解析与 RPC 传输成功! 耗时: " << xlsx_task_res.elapsed_ms << " ms\n";

  // -------------------------------------------------------------
  // Test 7: End-to-End Indexing & FTS5 Chinese Keyword Retrieval
  // -------------------------------------------------------------
  std::cout << "\n[Step 7] 将抽取的 Office 全文注入加密存储底座并执行 FTS5 检索...\n";
  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  CHECK(storage->Open(test_db, passphrase, dict_dir));

  // Index DOCX, XLSX, PPTX
  CHECK(storage->IndexDocument(1001, docx_path, "sample.docx", "docx", GetFileSize(docx_path), 1700000000, 1,
                               docx_shm_text));
  CHECK(storage->IndexDocument(1002, xlsx_path, "sample.xlsx", "xlsx", GetFileSize(xlsx_path), 1700000000, 1,
                               xlsx_shm_text));
  CHECK(
      storage->IndexDocument(1003, pptx_path, "sample.pptx", "pptx", GetFileSize(pptx_path), 1700000000, 1, pptx_text));

  // Search keyword in DOCX
  auto hits_ai = storage->Search("人工智能", 10, 0);
  CHECK(!hits_ai.empty());
  CHECK(hits_ai[0].doc_id == 1001);
  std::string ai_content;
  CHECK(storage->GetDocumentContent(hits_ai[0].doc_id, &ai_content));
  std::cout << "  [+] 检索 DOCX 成功: \"人工智能\" -> 命中: " << hits_ai[0].file_name
            << ", 摘要: " << soeasy::common::SnippetHighlighter::GenerateSnippet(ai_content, {"人工智能"}, 100) << "\n";

  // Search keyword in XLSX
  auto hits_finance = storage->Search("财务报表", 10, 0);
  CHECK(!hits_finance.empty());
  CHECK(hits_finance[0].doc_id == 1002);
  std::string finance_content;
  CHECK(storage->GetDocumentContent(hits_finance[0].doc_id, &finance_content));
  std::cout << "  [+] 检索 XLSX 成功: \"财务报表\" -> 命中: " << hits_finance[0].file_name
            << ", 摘要: " << soeasy::common::SnippetHighlighter::GenerateSnippet(finance_content, {"财务报表"}, 100)
            << "\n";

  // Search keyword in PPTX
  auto hits_roadshow = storage->Search("路演发布会", 10, 0);
  CHECK(!hits_roadshow.empty());
  CHECK(hits_roadshow[0].doc_id == 1003);
  std::string roadshow_content;
  CHECK(storage->GetDocumentContent(hits_roadshow[0].doc_id, &roadshow_content));
  std::cout << "  [+] 检索 PPTX 成功: \"路演发布会\" -> 命中: " << hits_roadshow[0].file_name
            << ", 摘要: " << soeasy::common::SnippetHighlighter::GenerateSnippet(roadshow_content, {"路演发布会"}, 100)
            << "\n";

  // -------------------------------------------------------------
  // Test 8: DocToText Smart File Logging Mode (Disabled vs Enabled)
  // -------------------------------------------------------------
  std::cout << "\n[Step 8] 测试 DocToText 智能日志开关与文件重定向...\n";
  const std::string dt_log_file = test_dir + "/doctotext_test.log";
  std::filesystem::remove(dt_log_file);

  // 1. Verify default disabled: no log file should be generated
  soeasy::worker::L2OfficeParser::SetLogging(false, dt_log_file);
  std::string dummy_pdf_text;
  parser.Parse(pdf_path, "pdf", &dummy_pdf_text);
  CHECK(!std::filesystem::exists(dt_log_file));
  std::cout << "  [+] 默认关闭模式验证通过: 无任何日志文件生成且控制台完全静音\n";

  // 2. Verify enabled: logs are cleanly captured to file
  soeasy::worker::L2OfficeParser::SetLogging(true, dt_log_file);
  parser.Parse(pdf_path, "pdf", &dummy_pdf_text);
  // Turn off logging to flush and release file handle before reading
  soeasy::worker::L2OfficeParser::SetLogging(false);

  CHECK(std::filesystem::exists(dt_log_file));
  CHECK(std::filesystem::file_size(dt_log_file) > 0);

  std::ifstream log_ifs(dt_log_file);
  std::string log_content((std::istreambuf_iterator<char>(log_ifs)), std::istreambuf_iterator<char>());
  log_ifs.close();
  CHECK(log_content.find("[DocToText Session Started]") != std::string::npos);
  std::string lower_log = log_content;
  std::transform(lower_log.begin(), lower_log.end(), lower_log.begin(), ::tolower);
  CHECK(lower_log.find("sample") != std::string::npos || lower_log.find(".pdf") != std::string::npos);
  std::cout << "  [+] 开启落盘模式验证通过: 成功捕获 DocToText 底层解析日志至文件! 大小: " << log_content.size()
            << " 字节\n";

  // -------------------------------------------------------------
  // Test 9: Standard OOXML Main Path, Abnormal XLSX Fallback & Concurrent Log Race
  // -------------------------------------------------------------
  std::cout << "\n[Step 9] 验证标准 OOXML 直通、异常 XLSX 兜底与并发临时日志安全...\n";
  const std::string ooxml_log_file = test_dir + "/ooxml_concurrent.log";
  std::filesystem::remove(ooxml_log_file);
  soeasy::worker::L2OfficeParser::SetLogging(true, ooxml_log_file);

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t]() {
      std::string text;
      const bool use_xlsx = (t % 2 == 0);
      bool ok = parser.Parse(use_xlsx ? xlsx_path : pptx_path, use_xlsx ? "xlsx" : "pptx", &text);
      CHECK(ok);
      CHECK(!text.empty());
    });
  }
  for (auto &th : threads) th.join();
  soeasy::worker::L2OfficeParser::SetLogging(false);

  {
    std::ifstream ooxml_ifs(ooxml_log_file);
    std::string ooxml_log((std::istreambuf_iterator<char>(ooxml_ifs)), std::istreambuf_iterator<char>());
    CHECK(ooxml_log.find("Could not locate main file") == std::string::npos);
    for (const auto &entry : std::filesystem::directory_iterator(test_dir)) {
      CHECK(entry.path().filename().string().find(".dt.") == std::string::npos);
    }
  }
  std::cout << "  [+] 标准 XLSX/PPTX 并发走 DocToText 主链路通过且无残留 .dt.*.tmp 文件\n";

  // Case A: Abnormal XLSX missing xl/workbook.xml -> falls back to ParseXlsx
  const std::string no_wb_xlsx = test_dir + "/missing_workbook.xlsx";
  {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    CHECK(mz_zip_writer_init_file(&zip, no_wb_xlsx.c_str(), 0));
    const std::string ss =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><sst><si><t>残缺工作簿兜底正文</t></si></sst>";
    mz_zip_writer_add_mem(&zip, "xl/sharedStrings.xml", ss.data(), ss.size(), MZ_BEST_COMPRESSION);
    CHECK(mz_zip_writer_finalize_archive(&zip));
    CHECK(mz_zip_writer_end(&zip));
  }
  std::string rescued_text;
  CHECK(parser.Parse(no_wb_xlsx, "xlsx", &rescued_text));
  CHECK(rescued_text.find("残缺工作簿兜底正文") != std::string::npos);

  // Case B: Abnormal empty XLSX with no sheets/strings -> returns false
  const std::string empty_xlsx = test_dir + "/empty_no_content.xlsx";
  {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    CHECK(mz_zip_writer_init_file(&zip, empty_xlsx.c_str(), 0));
    const std::string wb = "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"/>";
    mz_zip_writer_add_mem(&zip, "xl/workbook.xml", wb.data(), wb.size(), MZ_BEST_COMPRESSION);
    CHECK(mz_zip_writer_finalize_archive(&zip));
    CHECK(mz_zip_writer_end(&zip));
  }
  std::string empty_xlsx_text = "stale";
  CHECK(!parser.Parse(empty_xlsx, "xlsx", &empty_xlsx_text));
  CHECK(empty_xlsx_text.empty());

  // Case C: Corrupted non-ZIP XLSX -> returns false
  const std::string bad_xlsx = test_dir + "/corrupted.xlsx";
  {
    std::ofstream bad_ofs(bad_xlsx, std::ios::binary);
    bad_ofs << "NOT_A_VALID_ZIP_ARCHIVE";
  }
  std::string bad_xlsx_text = "stale";
  CHECK(!parser.Parse(bad_xlsx, "xlsx", &bad_xlsx_text));
  CHECK(bad_xlsx_text.empty());
  std::cout << "  [+] 异常 XLSX (缺 workbook.xml / 无内容 / 损坏文件) 兜底与失败语义验证通过\n";

  // -------------------------------------------------------------
  // Test 10: Embedded Image OCR End-to-End, Instance Isolation & CheckIdleUnload
  // -------------------------------------------------------------
  std::cout << "\n[Step 10] 验证 DOCX 内嵌图片 OCR 链路、解析器实例隔离与空闲卸载...\n";
  {
    struct EmbeddedOcrFlagGuard {
      ~EmbeddedOcrFlagGuard() { soeasy::worker::L2OfficeParser::SetEmbeddedImageOcr(false); }
    } ocr_flag_guard;

    cv::Mat ocr_img(600, 800, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::putText(ocr_img, "SoEasy Embedded OCR 2026", cv::Point(50, 140), cv::FONT_HERSHEY_SIMPLEX, 1.1,
                cv::Scalar(0, 0, 0), 2);
    cv::putText(ocr_img, "Office Pipeline Verification", cv::Point(50, 260), cv::FONT_HERSHEY_SIMPLEX, 1.1,
                cv::Scalar(0, 0, 0), 2);
    std::vector<uchar> png_bytes;
    CHECK(cv::imencode(".png", ocr_img, png_bytes));
    CHECK(png_bytes.size() >= 128);

    const std::string embedded_docx = test_dir + "/embedded_ocr.docx";
    {
      mz_zip_archive zip;
      std::memset(&zip, 0, sizeof(zip));
      CHECK(mz_zip_writer_init_file(&zip, embedded_docx.c_str(), 0));

      const std::string content_types =
          "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
          "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
          "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
          "  <Default Extension=\"png\" ContentType=\"image/png\"/>\n"
          "  <Override PartName=\"/word/document.xml\""
          " ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>\n"
          "</Types>";
      mz_zip_writer_add_mem(&zip, "[Content_Types].xml", content_types.data(), content_types.size(),
                            MZ_BEST_COMPRESSION);

      const std::string root_rels =
          "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
          "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>\n"
          "</Relationships>";
      mz_zip_writer_add_mem(&zip, "_rels/.rels", root_rels.data(), root_rels.size(), MZ_BEST_COMPRESSION);

      const std::string doc_rels =
          "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
          "  <Relationship Id=\"rIdImg1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image1.png\"/>\n"
          "</Relationships>";
      mz_zip_writer_add_mem(&zip, "word/_rels/document.xml.rels", doc_rels.data(), doc_rels.size(),
                            MZ_BEST_COMPRESSION);
      mz_zip_writer_add_mem(&zip, "word/media/image1.png", png_bytes.data(), png_bytes.size(), MZ_BEST_COMPRESSION);

      const std::string doc_xml =
          "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
          " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
          " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">\n"
          "  <w:body>\n"
          "    <w:p><w:r><w:t>EmbeddedAnchorText</w:t></w:r></w:p>\n"
          "    <w:p><w:r><w:drawing><a:blip r:embed=\"rIdImg1\"/></w:drawing></w:r></w:p>\n"
          "  </w:body>\n"
          "</w:document>";
      mz_zip_writer_add_mem(&zip, "word/document.xml", doc_xml.data(), doc_xml.size(), MZ_BEST_COMPRESSION);

      CHECK(mz_zip_writer_finalize_archive(&zip));
      CHECK(mz_zip_writer_end(&zip));
    }

    soeasy::worker::L2OfficeParser embedded_parser;
    soeasy::worker::L2OfficeParser isolated_parser;

    soeasy::worker::L2OfficeParser::SetEmbeddedImageOcr(false);
    std::string text_disabled;
    CHECK(embedded_parser.Parse(embedded_docx, "docx", &text_disabled));
    CHECK(text_disabled.find("EmbeddedAnchorText") != std::string::npos);
    CHECK(!embedded_parser.is_embedded_ocr_loaded());

    soeasy::worker::L2OfficeParser::SetEmbeddedImageOcr(true);
    std::string text_enabled;
    CHECK(embedded_parser.Parse(embedded_docx, "docx", &text_enabled));
    CHECK(text_enabled.find("EmbeddedAnchorText") != std::string::npos);

    std::string lower_ocr = text_enabled;
    std::transform(lower_ocr.begin(), lower_ocr.end(), lower_ocr.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(lower_ocr.find("soeasy") != std::string::npos || lower_ocr.find("2026") != std::string::npos ||
          lower_ocr.find("office") != std::string::npos);
    CHECK(embedded_parser.is_embedded_ocr_loaded());
    CHECK(!isolated_parser.is_embedded_ocr_loaded());

    embedded_parser.CheckIdleUnload(0.0);
    CHECK(!embedded_parser.is_embedded_ocr_loaded());
    std::cout << "  [+] DOCX 内嵌图片 OCR 回调 (user_data 透传)、实例隔离及 CheckIdleUnload(0.0) 卸载验证通过\n";
  }

  // Cleanup
  worker_guard.reset();
  storage->Close();
  storage.reset();

  std::error_code ec;
  std::filesystem::remove_all(test_dir, ec);
  std::remove(test_db.c_str());
  std::remove((test_db + "-wal").c_str());
  std::remove((test_db + "-shm").c_str());

  std::cout << "\n========================================================\n";
  std::cout << "   [PASS] 全部 Phase 5 L2 办公文档深度解析实证测试验证通过!\n";
  std::cout << "========================================================\n";
  return 0;
}
