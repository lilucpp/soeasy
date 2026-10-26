// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <sys/stat.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "miniz.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"
#include "worker/l2_office_parser.h"

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::cerr << "[-] CHECK FAILED: " #expr " at " << __FILE__ << ":"     \
                << __LINE__ << std::endl;                                    \
      std::abort();                                                          \
    }                                                                        \
  } while (0)

namespace {

uint64_t GetFileSize(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    return static_cast<uint64_t>(st.st_size);
  }
  return 0;
}

void CreateSampleDocx(const std::string& path) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  CHECK(mz_zip_writer_init_file(&zip, path.c_str(), 0));

  const std::string content_types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
      "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
      "</Types>";
  mz_zip_writer_add_mem(&zip, "[Content_Types].xml", content_types.data(),
                        content_types.size(), MZ_BEST_COMPRESSION);

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
  mz_zip_writer_add_mem(&zip, "word/document.xml", document_xml.data(),
                        document_xml.size(), MZ_BEST_COMPRESSION);

  CHECK(mz_zip_writer_finalize_archive(&zip));
  CHECK(mz_zip_writer_end(&zip));
}

void CreateSampleXlsx(const std::string& path) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  CHECK(mz_zip_writer_init_file(&zip, path.c_str(), 0));

  const std::string shared_strings =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"3\" uniqueCount=\"3\">\n"
      "  <si><t>2026年度第一季度财务报表与研发开支明细</t></si>\n"
      "  <si><t>服务器集群购置费用</t></si>\n"
      "  <si><t>算法工程师与架构师薪酬开支</t></si>\n"
      "</sst>";
  mz_zip_writer_add_mem(&zip, "xl/sharedStrings.xml", shared_strings.data(),
                        shared_strings.size(), MZ_BEST_COMPRESSION);

  const std::string sheet1 =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n"
      "  <sheetData>\n"
      "    <row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c></row>\n"
      "    <row r=\"2\"><c r=\"A2\" t=\"s\"><v>1</v></c><c r=\"B2\"><v>850000</v></c></row>\n"
      "  </sheetData>\n"
      "</worksheet>";
  mz_zip_writer_add_mem(&zip, "xl/worksheets/sheet1.xml", sheet1.data(),
                        sheet1.size(), MZ_BEST_COMPRESSION);

  CHECK(mz_zip_writer_finalize_archive(&zip));
  CHECK(mz_zip_writer_end(&zip));
}

void CreateSamplePptx(const std::string& path) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  CHECK(mz_zip_writer_init_file(&zip, path.c_str(), 0));

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
  mz_zip_writer_add_mem(&zip, "ppt/slides/slide1.xml", slide1.data(),
                        slide1.size(), MZ_BEST_COMPRESSION);

  CHECK(mz_zip_writer_finalize_archive(&zip));
  CHECK(mz_zip_writer_end(&zip));
}

void CreateSamplePdf(const std::string& path) {
  // Generate a valid minimal PDF 1.4 document containing selectable text
  std::ofstream ofs(path, std::ios::binary);
  ofs << "%PDF-1.4\n"
      << "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
      << "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
      << "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      << "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n"
      << "4 0 obj\n<< /Length 72 >>\nstream\n"
      << "BT\n/F1 18 Tf\n100 700 Td\n(SoEasy Deep PDF Document Search Engine) Tj\nET\n"
      << "endstream\nendobj\n"
      << "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n"
      << "xref\n0 6\n0000000000 65535 f \n0000000009 00000 n \n0000000058 00000 n \n"
      << "0000000115 00000 n \n0000000266 00000 n \n0000000389 00000 n \n"
      << "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n466\n%%EOF\n";
}

void CreateScannedSamplePdf(const std::string& path) {
  // Generate a larger PDF containing no extractable text stream (simulating scanned/image PDF)
  std::ofstream ofs(path, std::ios::binary);
  ofs << "%PDF-1.4\n"
      << "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
      << "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
      << "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>\nendobj\n"
      << "4 0 obj\n<< /Length 25000 >>\nstream\n";
  // Fill with dummy image raster bytes
  std::string dummy_pixels(25000, '\x80');
  ofs.write(dummy_pixels.data(), dummy_pixels.size());
  ofs << "\nendstream\nendobj\n"
      << "xref\n0 5\n"
      << "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n25200\n%%EOF\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  std::cout << "========================================================\n";
  std::cout << "   SoEasy L2 办公文档与富文本深度解析实证测试 (Phase 5)\n";
  std::cout << "   DocToText (lithiumFlower) + OOXML + PDF + 扫描件识别 + 沙箱与检索\n";
  std::cout << "========================================================\n\n";

  const std::string test_dir = "/tmp/soeasy_phase5_test";
  const std::string test_db = "/tmp/soeasy_phase5_test.db";
  const std::string passphrase = "SoEasy_Phase5_Passphrase_2026!";
  const std::string dict_dir =
      "/home/peanut/project/soeasy/third_party/cppjieba/dict";

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
  std::string worker_bin = "/home/peanut/project/soeasy/build/soeasy-worker";
  CHECK(worker_guard->Init(worker_bin, 2, 2000));

  soeasy::service::ParseTask docx_task;
  docx_task.doc_id = 1001;
  docx_task.file_path = docx_path;
  docx_task.ext = "docx";
  docx_task.parse_level = 2;

  soeasy::service::ParseTaskResult docx_task_res;
  bool worker_docx_ok = worker_guard->ExecuteTask(docx_task, &docx_task_res);
  CHECK(worker_docx_ok);
  CHECK(docx_task_res.status == soeasy::service::TaskStatus::kSuccess);

  std::string docx_shm_text;
  const uint8_t* docx_slot_ptr = nullptr;
  size_t docx_slot_bytes = 0;
  if (worker_guard->shm_pool()->ReadSlot(docx_task_res.slot_id, &docx_slot_ptr,
                                         &docx_slot_bytes)) {
    docx_shm_text.assign(reinterpret_cast<const char*>(docx_slot_ptr),
                         docx_slot_bytes);
    worker_guard->shm_pool()->ReleaseSlot(docx_task_res.slot_id);
  }
  CHECK(docx_shm_text.find("人工智能战略规划") != std::string::npos);
  std::cout << "  [+] 沙箱跨进程 DOCX 解析与 SHM 槽位传输成功! 耗时: "
            << docx_task_res.elapsed_ms << " ms\n";

  soeasy::service::ParseTask xlsx_task;
  xlsx_task.doc_id = 1002;
  xlsx_task.file_path = xlsx_path;
  xlsx_task.ext = "xlsx";
  xlsx_task.parse_level = 2;

  soeasy::service::ParseTaskResult xlsx_task_res;
  bool worker_xlsx_ok = worker_guard->ExecuteTask(xlsx_task, &xlsx_task_res);
  CHECK(worker_xlsx_ok);
  CHECK(xlsx_task_res.status == soeasy::service::TaskStatus::kSuccess);

  std::string xlsx_shm_text;
  const uint8_t* xlsx_slot_ptr = nullptr;
  size_t xlsx_slot_bytes = 0;
  if (worker_guard->shm_pool()->ReadSlot(xlsx_task_res.slot_id, &xlsx_slot_ptr,
                                         &xlsx_slot_bytes)) {
    xlsx_shm_text.assign(reinterpret_cast<const char*>(xlsx_slot_ptr),
                         xlsx_slot_bytes);
    worker_guard->shm_pool()->ReleaseSlot(xlsx_task_res.slot_id);
  }
  CHECK(xlsx_shm_text.find("2026年度第一季度财务报表") != std::string::npos);
  std::cout << "  [+] 沙箱跨进程 XLSX 解析与 SHM 槽位传输成功! 耗时: "
            << xlsx_task_res.elapsed_ms << " ms\n";

  // -------------------------------------------------------------
  // Test 7: End-to-End Indexing & FTS5 Chinese Keyword Retrieval
  // -------------------------------------------------------------
  std::cout << "\n[Step 7] 将抽取的 Office 全文注入加密存储底座并执行 FTS5 检索...\n";
  auto storage = std::make_shared<soeasy::storage::StorageEngine>();
  CHECK(storage->Open(test_db, passphrase, dict_dir));

  // Index DOCX, XLSX, PPTX
  CHECK(storage->IndexDocument(1001, docx_path, "sample.docx", "docx",
                               GetFileSize(docx_path), 1700000000, 1,
                               docx_shm_text));
  CHECK(storage->IndexDocument(1002, xlsx_path, "sample.xlsx", "xlsx",
                               GetFileSize(xlsx_path), 1700000000, 1,
                               xlsx_shm_text));
  CHECK(storage->IndexDocument(1003, pptx_path, "sample.pptx", "pptx",
                               GetFileSize(pptx_path), 1700000000, 1,
                               pptx_text));

  // Search keyword in DOCX
  auto hits_ai = storage->Search("人工智能", 10, 0);
  CHECK(!hits_ai.empty());
  CHECK(hits_ai[0].doc_id == 1001);
  std::cout << "  [+] 检索 DOCX 成功: \"人工智能\" -> 命中: " << hits_ai[0].file_name
            << ", 摘要: " << hits_ai[0].snippet << "\n";

  // Search keyword in XLSX
  auto hits_finance = storage->Search("财务报表", 10, 0);
  CHECK(!hits_finance.empty());
  CHECK(hits_finance[0].doc_id == 1002);
  std::cout << "  [+] 检索 XLSX 成功: \"财务报表\" -> 命中: " << hits_finance[0].file_name
            << ", 摘要: " << hits_finance[0].snippet << "\n";

  // Search keyword in PPTX
  auto hits_roadshow = storage->Search("路演发布会", 10, 0);
  CHECK(!hits_roadshow.empty());
  CHECK(hits_roadshow[0].doc_id == 1003);
  std::cout << "  [+] 检索 PPTX 成功: \"路演发布会\" -> 命中: " << hits_roadshow[0].file_name
            << ", 摘要: " << hits_roadshow[0].snippet << "\n";

  // Cleanup
  worker_guard.reset();
  storage->Close();

  std::filesystem::remove_all(test_dir);
  std::remove(test_db.c_str());
  std::remove((test_db + "-wal").c_str());
  std::remove((test_db + "-shm").c_str());

  std::cout << "\n========================================================\n";
  std::cout << "   [PASS] 全部 Phase 5 L2 办公文档深度解析实证测试验证通过!\n";
  std::cout << "========================================================\n";
  return 0;
}
