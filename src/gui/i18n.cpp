#include "gui/i18n.h"

namespace soeasy {
namespace gui {

I18n& I18n::Instance() {
  static I18n instance;
  return instance;
}

I18n::I18n() {
  InitDictionary();
}

void I18n::SetLanguage(const std::string& lang_code) {
  if (lang_code == "en") {
    current_lang_ = Language::kEnglish;
  } else {
    current_lang_ = Language::kChinese;
  }
}

std::string I18n::GetLanguageCode() const {
  return current_lang_ == Language::kEnglish ? "en" : "zh";
}

QString I18n::Get(const char* key) const {
  if (!key) return QString();
  const auto& dict = (current_lang_ == Language::kEnglish) ? en_dict_ : zh_dict_;
  auto it = dict.find(key);
  if (it != dict.end()) {
    return QString::fromUtf8(it->second.c_str());
  }
  // Fallback to key or Chinese
  auto it_zh = zh_dict_.find(key);
  if (it_zh != zh_dict_.end()) {
    return QString::fromUtf8(it_zh->second.c_str());
  }
  return QString::fromUtf8(key);
}

QString I18n::GetCategoryName(size_t cat_index, const QString& default_name) const {
  std::string key = "cat_name_" + std::to_string(cat_index);
  QString val = Get(key.c_str());
  return val.isEmpty() ? default_name : val;
}

QString I18n::GetCategoryDesc(size_t cat_index, const QString& default_desc) const {
  std::string key = "cat_desc_" + std::to_string(cat_index);
  QString val = Get(key.c_str());
  return val.isEmpty() ? default_desc : val;
}

QString I18n::GetFormatDesc(const QString& ext, const QString& default_desc) const {
  if (current_lang_ == Language::kChinese) {
    return default_desc;
  }
  std::string key = "fmt_desc_" + ext.toLower().toStdString();
  auto it = en_dict_.find(key);
  if (it != en_dict_.end()) {
    return QString::fromUtf8(it->second.c_str());
  }
  return default_desc;
}

void I18n::InitDictionary() {
  // -----------------------------------------------------------
  // Chinese Dictionary
  // -----------------------------------------------------------
  zh_dict_ = {
      // Common & Window
      {"app_title", "SoEasy - 让电脑没有难搜的文件"},
      {"minimize", "最小化"},
      {"maximize", "最大化 / 还原"},
      {"close", "关闭"},
      {"browse", "浏览..."},
      {"msg_tip", "提示"},
      {"save_apply", "保存并应用"},
      {"cancel", "取消"},

      // Search Bar
      {"search_placeholder", "输入搜索关键词（支持 FTS5 语法：AND, OR, NOT, \"精确词组\", 前缀*）..."},
      {"mode_fulltext", "全文倒排 (FTS5)"},
      {"mode_filename", "文件名秒搜"},
      {"btn_preview", "◨ 预览"},
      {"tip_preview", "切换右侧即时正文预览面板 (Ctrl+P)"},
      {"btn_settings", "⚙ 设置中心"},
      {"label_scope", "限定搜索路径:"},
      {"scope_placeholder", "留空表示全盘/全局检索；填入路径则仅在该目录及其子树下检索..."},
      {"btn_clear_scope", "清除范围"},
      {"dlg_select_scope", "选择限定搜索目录"},

      // Result Table
      {"th_id", "序号"},
      {"th_filename", "文件名称"},
      {"th_path", "所在路径"},
      {"th_size", "大小"},
      {"th_mtime", "修改时间"},
      {"th_score", "相关度评分"},
      {"filename_match_prefix", "文件名匹配: %1"},

      // Preview Panel
      {"no_file_selected", "未选中文件"},
      {"btn_open_file", "打开文件"},
      {"btn_open_folder", "所在目录"},
      {"label_jump", "关键词跳转:"},
      {"search_in_content_ph", "在正文中查找..."},
      {"btn_prev", "▲ 上一个"},
      {"btn_next", "▼ 下一个"},
      {"match_count", "匹配: %1 / %2"},
      {"no_content_extracted", "无正文提取内容或尚未建立正文索引"},

      // Status Bar
      {"status_ready", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> 就绪"},
      {"status_stats", "在索引文件: %1 个 | 匹配结果: %2 条"},
      {"status_latency", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> 检索耗时: %1 ms"},
      {"btn_cancel_index", "✕ 取消"},
      {"tip_cancel_index", "取消当前正在进行的索引任务"},
      {"status_cancelling", "<span style=\"color:#D97706; font-weight:bold;\">! 正在协同取消索引...</span>"},
      {"status_indexing", "正在扫描并建立倒排索引: %1 / %2 (%3%)"},
      {"status_indexed_complete", "全量索引已完成: 共 %1 个文档，耗时 %2 秒"},
      {"status_indexed_cancel", "索引已取消: 共处理 %1 个文档，耗时 %2 秒"},

      // Settings Dialog
      {"settings_title", "SoEasy - 索引配置与设置中心"},
      {"tab_general", "常规与语言"},
      {"tab_dirs", "索引目录"},
      {"tab_patterns", "排除目录规则"},
      {"tab_exts", "文件格式与解析"},
      {"tab_perf", "性能与日志"},

      // Settings - General Tab
      {"label_language", "界面显示语言 (UI Language):"},
      {"lang_zh", "简体中文 (Chinese)"},
      {"lang_en", "English"},
      {"lang_hint", "💡 切换后界面文字将立即动态刷新为所选语言。"},

      // Settings - Dirs Tab
      {"label_dirs", "系统将实时监控并自动为以下目录及其子文件夹建立索引："},
      {"btn_add_dir", "添加目录..."},
      {"btn_rm_dir", "移除所选"},
      {"dlg_select_dir", "选择待索引的目录"},

      // Settings - Patterns Tab
      {"label_patterns", "匹配以下路径规则的目录将被自动忽略（不建立倒排索引）："},
      {"btn_add_pattern", "添加规则"},
      {"pattern_ph", "例如: /.git/ 或 /node_modules/"},

      // Settings - Extensions Tab
      {"group_ocr", "高级解析能力"},
      {"check_embedded_ocr", "提取并 OCR 解析文档内嵌图片 (Office 系列 / PDF / ODF 等)"},
      {"tip_embedded_ocr", "💡 默认不勾选。勾选后将提取 Office/PDF/ODF 等文档中的嵌入插图并调用 PP-OCRv6 引擎进行文本识别融合；\n    不勾选可获得极致解析性能（纯文本毫秒级完成索引）。"},
      {"label_tree_title", "支持与索引的文件格式分类（勾选以启用全文索引）："},
      {"tree_header_col0", "分类 / 格式扩展名"},
      {"tree_header_col1", "说明与处理引擎"},
      {"custom_category_name", "用户自定义格式 (Custom)"},
      {"custom_category_desc", "用户手动添加的专有文件后缀与提取规则"},
      {"custom_ext_desc", "用户自定义格式"},
      {"format_planned", " (规划中，暂未支持)"},
      {"format_planned_tip", "该格式正文检索功能规划中，暂未支持"},
      {"label_add_custom", "添加自定义扩展名:"},
      {"custom_ext_ph", "例如: vue 或 proto (无需加点)"},
      {"btn_add_custom", "添加格式"},
      {"btn_rm_custom", "移除所选自定义"},
      {"btn_select_all", "全选"},
      {"btn_deselect_all", "全不选"},
      {"btn_reset_exts", "恢复推荐默认"},
      {"exts_count", "已启用: %1 / %2 种文件格式"},
      {"msg_select_custom_first", "请先在分类列表中选择要删除的自定义格式。"},
      {"msg_preset_cannot_delete", "预置的标准格式无法删除；如需停用，请取消该格式前的勾选框。"},

      // Settings - Performance Tab
      {"label_debounce", "文件变动去抖动等待延迟:"},
      {"suffix_ms", " 毫秒"},
      {"label_max_file_size", "单文件最大全文索引上限:"},
      {"suffix_mb", " MB"},
      {"label_workers", "并发解析 Worker 进程数:"},
      {"suffix_workers", " 个进程"},
      {"label_min_ocr_chars", "图片最少有效文字数（过滤表情/图标）:"},
      {"suffix_min_chars", " 个字 (0为不限制)"},
      {"check_filter_thumbs", "自动忽略应用缓存缩略图与图标 (如 WeAppIcon, Thumb)"},
      {"label_log_level", "运行日志详细级别:"},
      {"log_debug", "DEBUG (详细调试与耗时跟踪)"},
      {"log_info", "INFO  (标准运行信息，默认)"},
      {"log_warn", "WARN  (生产精简模式，仅警告/错误)"},
      {"log_error", "ERROR (仅严重故障记录)"},
      {"label_log_console", "终端输出调试:"},
      {"check_log_console", "输出控制台实时镜像 (Console stdout，用于终端调试)"},
      {"label_log_sync", "落盘同步模式:"},
      {"check_log_sync", "同步即时落盘模式 (Sync Flush，每条日志即刻刷盘，排查异常卡死)"},
      {"label_dt_log", "DocToText 日志:"},
      {"check_dt_log", "启用 DocToText 详细文档解析日志 (默认关闭，用于排查 Office/PDF 解析问题)"},
      {"label_dt_path", "日志输出文件路径:"},
      {"dt_path_ph", "留空则默认输出至 logs/doctotext.log"},
      {"dlg_select_log", "选择 DocToText 日志输出文件"},

      // Rebuild & Actions
      {"btn_rebuild", "清空并重建索引..."},
      {"rebuild_confirm_title", "确认重建索引"},
      {"rebuild_confirm_text", "此操作将彻底清空当前所有的全文倒排索引与密态缓存，\n并对所有配置的监控目录从头进行全量重新解析与构建。\n\n确定要继续吗？"},

      // Content Preview Dialog
      {"preview_dlg_title", "文档正文预览 - %1"},
      {"btn_open_folder_sys", "打开所在文件夹"},
      {"btn_open_app_sys", "用系统默认程序打开"},

      // Categories (0 to 13)
      {"cat_name_0", "Microsoft Office 新版格式 (OOXML)"},
      {"cat_desc_0", "基于 OpenXML 标准的现代化办公文档"},
      {"cat_name_1", "Microsoft Office 经典二进制格式"},
      {"cat_desc_1", "Microsoft Office 97-2003 经典二进制格式"},
      {"cat_name_2", "开放文档格式 (ODF / LibreOffice)"},
      {"cat_desc_2", "OASIS OpenDocument 开放文档标准"},
      {"cat_name_3", "便携式文档 (PDF)"},
      {"cat_desc_3", "Adobe PDF 电子出版物与矢量文档"},
      {"cat_name_4", "网页与富文本文档"},
      {"cat_desc_4", "超文本网页与跨平台富文本格式"},
      {"cat_name_5", "电子邮件格式 (Email)"},
      {"cat_desc_5", "MIME 规范电子邮件及附件"},
      {"cat_name_6", "独立图像格式 (PP-OCRv6 图像文字识别)"},
      {"cat_desc_6", "包含深度学习光学字符识别能力的图像文件"},
      {"cat_name_7", "苹果办公套件 (Apple iWork)"},
      {"cat_desc_7", "Apple macOS/iOS 办公套件文档"},
      {"cat_name_8", "编程语言与脚本源代码"},
      {"cat_desc_8", "主流编程语言源文件与系统运维脚本"},
      {"cat_name_9", "XML 标记语言家族"},
      {"cat_desc_9", "XML 格式及其模式与转换定义"},
      {"cat_name_10", "逗号分隔值表格 (CSV)"},
      {"cat_desc_10", "轻量纯文本结构化表格数据"},
      {"cat_name_11", "结构化数据与配置文件"},
      {"cat_desc_11", "常见系统配置、应用参数与结构化描述文件"},
      {"cat_name_12", "纯文本与日志文件"},
      {"cat_desc_12", "通用纯文本文档与运行日志记录"},
      {"cat_name_13", "压缩归档格式 (Archives)"},
      {"cat_desc_13", "打包与压缩文件容器 (正文递归检索规划中)"},
      {"cat_disabled_tip", "该分类正文检索功能规划中，暂未支持"},
      {"exts_count_status", "已启用: %1 / %2 种文件格式"},
      {"custom_format_desc", "用户手动添加的专有文件后缀与提取规则"},
  };

  // -----------------------------------------------------------
  // English Dictionary
  // -----------------------------------------------------------
  en_dict_ = {
      // Common & Window
      {"app_title", "SoEasy - Make Every File Searchable"},
      {"minimize", "Minimize"},
      {"maximize", "Maximize / Restore"},
      {"close", "Close"},
      {"browse", "Browse..."},
      {"msg_tip", "Notice"},
      {"save_apply", "Save && Apply"},
      {"cancel", "Cancel"},

      // Search Bar
      {"search_placeholder", "Enter search keywords (Supports FTS5: AND, OR, NOT, \"exact phrase\", prefix*)..."},
      {"mode_fulltext", "Full-Text (FTS5)"},
      {"mode_filename", "Filename Search"},
      {"btn_preview", "◨ Preview"},
      {"tip_preview", "Toggle preview pane (Ctrl+P)"},
      {"btn_settings", "⚙ Settings"},
      {"label_scope", "Search Scope:"},
      {"scope_placeholder", "Leave empty for global search; or enter path to search within directory tree..."},
      {"btn_clear_scope", "Clear Scope"},
      {"dlg_select_scope", "Select Directory for Search Scope"},

      // Result Table
      {"th_id", "#"},
      {"th_filename", "File Name"},
      {"th_path", "Path"},
      {"th_size", "Size"},
      {"th_mtime", "Modified Date"},
      {"th_score", "Relevance"},
      {"filename_match_prefix", "Filename match: %1"},

      // Preview Panel
      {"no_file_selected", "No file selected"},
      {"btn_open_file", "Open File"},
      {"btn_open_folder", "Open Folder"},
      {"label_jump", "Jump to Match:"},
      {"search_in_content_ph", "Find in content..."},
      {"btn_prev", "▲ Previous"},
      {"btn_next", "▼ Next"},
      {"match_count", "Match: %1 / %2"},
      {"no_content_extracted", "No text extracted or document not yet indexed"},

      // Status Bar
      {"status_ready", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> Ready"},
      {"status_stats", "Indexed files: %1 | Matches: %2"},
      {"status_latency", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> Search time: %1 ms"},
      {"btn_cancel_index", "✕ Cancel"},
      {"tip_cancel_index", "Cancel current indexing task"},
      {"status_cancelling", "<span style=\"color:#D97706; font-weight:bold;\">! Cancelling indexing...</span>"},
      {"status_indexing", "Indexing in progress: %1 / %2 (%3%)"},
      {"status_indexed_complete", "Indexing completed: %1 documents in %2 s"},
      {"status_indexed_cancel", "Indexing cancelled: %1 documents processed in %2 s"},

      // Settings Dialog
      {"settings_title", "SoEasy - Settings Center"},
      {"tab_general", "General && Language"},
      {"tab_dirs", "Indexed Directories"},
      {"tab_patterns", "Excluded Patterns"},
      {"tab_exts", "File Formats"},
      {"tab_perf", "Performance && Logs"},

      // Settings - General Tab
      {"label_language", "Interface Language (UI Language):"},
      {"lang_zh", "简体中文 (Chinese)"},
      {"lang_en", "English"},
      {"lang_hint", "💡 Interface labels and components will update dynamically upon selection."},

      // Settings - Dirs Tab
      {"label_dirs", "The system automatically monitors and indexes the following directories:"},
      {"btn_add_dir", "Add Directory..."},
      {"btn_rm_dir", "Remove Selected"},
      {"dlg_select_dir", "Select Directory to Index"},

      // Settings - Patterns Tab
      {"label_patterns", "Directories matching the following patterns will be ignored (no indexing):"},
      {"btn_add_pattern", "Add Pattern"},
      {"pattern_ph", "e.g. /.git/ or /node_modules/"},

      // Settings - Extensions Tab
      {"group_ocr", "Advanced Parsing Capabilities"},
      {"check_embedded_ocr", "Extract && OCR embedded images in documents (Office / PDF / ODF)"},
      {"tip_embedded_ocr", "💡 Unchecked by default. Extracts embedded images for PP-OCRv6 recognition.\n    Unchecked yields maximum indexing throughput (instant plain-text extraction)."},
      {"label_tree_title", "Supported file format categories (check to enable full-text indexing):"},
      {"tree_header_col0", "Category / Extension"},
      {"tree_header_col1", "Description & Parser Engine"},
      {"custom_category_name", "User-Defined Formats (Custom)"},
      {"custom_category_desc", "Manually registered file extensions and extraction rules"},
      {"custom_ext_desc", "User-defined format"},
      {"format_planned", " (Planned, not yet supported)"},
      {"format_planned_tip", "Full-text indexing for this format is planned"},
      {"label_add_custom", "Add Custom Extension:"},
      {"custom_ext_ph", "e.g. vue or proto (without dot)"},
      {"btn_add_custom", "Add Format"},
      {"btn_rm_custom", "Remove Selected"},
      {"btn_select_all", "Select All"},
      {"btn_deselect_all", "Deselect All"},
      {"btn_reset_exts", "Reset to Default"},
      {"exts_count", "Enabled: %1 / %2 file formats"},
      {"msg_select_custom_first", "Please select the custom extension to delete from the list first."},
      {"msg_preset_cannot_delete", "Preset standard formats cannot be deleted. Uncheck the box to disable."},

      // Settings - Performance Tab
      {"label_debounce", "File change debounce delay:"},
      {"suffix_ms", " ms"},
      {"label_max_file_size", "Max indexable file size:"},
      {"suffix_mb", " MB"},
      {"label_workers", "Concurrent parser workers:"},
      {"suffix_workers", " workers"},
      {"label_min_ocr_chars", "Min OCR valid character count:"},
      {"suffix_min_chars", " chars (0 = unlimited)"},
      {"check_filter_thumbs", "Filter out cache thumbnails & icons (e.g. WeAppIcon, Thumb)"},
      {"label_log_level", "Runtime log verbosity level:"},
      {"log_debug", "DEBUG (Verbose trace & timing)"},
      {"log_info", "INFO  (Standard operational logs, default)"},
      {"log_warn", "WARN  (Warnings & errors only)"},
      {"log_error", "ERROR (Severe faults only)"},
      {"label_log_console", "Console output:"},
      {"check_log_console", "Mirror logs to console stdout (terminal debugging)"},
      {"label_log_sync", "Disk sync mode:"},
      {"check_log_sync", "Synchronous flush mode (immediate disk sync for crash diagnosis)"},
      {"label_dt_log", "DocToText logging:"},
      {"check_dt_log", "Enable DocToText verbose logging (troubleshooting Office/PDF parsing)"},
      {"label_dt_path", "Log file output path:"},
      {"dt_path_ph", "Leave empty for default logs/doctotext.log"},
      {"dlg_select_log", "Select DocToText Log File"},

      // Rebuild & Actions
      {"btn_rebuild", "Rebuild Index..."},
      {"rebuild_confirm_title", "Confirm Index Rebuild"},
      {"rebuild_confirm_text", "This will clear all inverted indices and encrypted caches,\nand completely rebuild indices from scratch for all configured directories.\n\nAre you sure you want to proceed?"},

      // Content Preview Dialog
      {"preview_dlg_title", "Content Preview - %1"},
      {"btn_open_folder_sys", "Open Folder"},
      {"btn_open_app_sys", "Open with Default App"},

      // Categories (0 to 13)
      {"cat_name_0", "Microsoft Office OOXML Formats"},
      {"cat_desc_0", "Modern documents based on OpenXML standard"},
      {"cat_name_1", "Microsoft Office Classic Binary Formats"},
      {"cat_desc_1", "Legacy Microsoft Office 97-2003 binary formats"},
      {"cat_name_2", "OpenDocument Formats (ODF / LibreOffice)"},
      {"cat_desc_2", "OASIS OpenDocument standard office formats"},
      {"cat_name_3", "Portable Document Format (PDF)"},
      {"cat_desc_3", "Adobe PDF publications and vector documents"},
      {"cat_name_4", "Web & Rich Text Documents"},
      {"cat_desc_4", "Hypertext documents and cross-platform rich text"},
      {"cat_name_5", "Email Messages & Attachments (Email)"},
      {"cat_desc_5", "MIME-compliant email messages and attachments"},
      {"cat_name_6", "Standalone Image Formats (PP-OCRv6)"},
      {"cat_desc_6", "Images parsed via deep learning optical character recognition"},
      {"cat_name_7", "Apple iWork Suite"},
      {"cat_desc_7", "Apple macOS/iOS office suite documents"},
      {"cat_name_8", "Source Code & Scripts"},
      {"cat_desc_8", "Major programming language source files and scripts"},
      {"cat_name_9", "XML Markup Language Family"},
      {"cat_desc_9", "XML schemas, transformations, and definitions"},
      {"cat_name_10", "Comma-Separated Values (CSV)"},
      {"cat_desc_10", "Lightweight plain-text structured tabular data"},
      {"cat_name_11", "Structured Data & Configurations"},
      {"cat_desc_11", "Configuration files, system settings, and data objects"},
      {"cat_name_12", "Plain Text & Logs"},
      {"cat_desc_12", "General plain text documents and runtime logs"},
      {"cat_name_13", "Compressed Archive Formats (Archives)"},
      {"cat_desc_13", "Archive packages (recursive archive search planned)"},
      {"cat_disabled_tip", "Search inside archives is planned and not yet supported"},
      {"exts_count_status", "Enabled: %1 / %2 file formats"},
      {"custom_format_desc", "User-defined file extensions and extraction rules"},

      // Format Descriptions (en)
      {"fmt_desc_docx", "Word 2007+ Document"},
      {"fmt_desc_xlsx", "Excel 2007+ Spreadsheet"},
      {"fmt_desc_pptx", "PowerPoint 2007+ Presentation"},
      {"fmt_desc_doc", "Word 97-2003 Document"},
      {"fmt_desc_xls", "Excel 97-2003 Workbook"},
      {"fmt_desc_ppt", "PowerPoint 97-2003 Presentation"},
      {"fmt_desc_xlsb", "Excel Binary Workbook"},
      {"fmt_desc_odt", "OpenDocument Text Document"},
      {"fmt_desc_ods", "OpenDocument Spreadsheet"},
      {"fmt_desc_odp", "OpenDocument Presentation"},
      {"fmt_desc_odg", "OpenDocument Drawing Document"},
      {"fmt_desc_fodt", "Flat XML ODF Text Document"},
      {"fmt_desc_fods", "Flat XML ODF Spreadsheet"},
      {"fmt_desc_fodp", "Flat XML ODF Presentation"},
      {"fmt_desc_pdf", "Adobe PDF Portable Document"},
      {"fmt_desc_html", "HTML HyperText Markup Language"},
      {"fmt_desc_htm", "HTM Web Page File"},
      {"fmt_desc_css", "Cascading Style Sheet (CSS)"},
      {"fmt_desc_rtf", "Rich Text Format Document"},
      {"fmt_desc_eml", "RFC 822 Email Message & Attachment"},
      {"fmt_desc_jpg", "JPEG Standard Image"},
      {"fmt_desc_jpeg", "JPEG Standard Image"},
      {"fmt_desc_png", "PNG Portable Network Graphics"},
      {"fmt_desc_webp", "Google WebP Efficient Image"},
      {"fmt_desc_bmp", "Windows Bitmap Image"},
      {"fmt_desc_tiff", "TIFF Tagged Image File"},
      {"fmt_desc_tif", "TIF Tagged Image File"},
      {"fmt_desc_jfif", "JPEG File Interchange Format"},
      {"fmt_desc_pnm", "Portable Anymap Image"},
      {"fmt_desc_pages", "Apple Pages Document"},
      {"fmt_desc_numbers", "Apple Numbers Spreadsheet"},
      {"fmt_desc_key", "Apple Keynote Presentation"},
      {"fmt_desc_keynote", "Apple Keynote Presentation"},
      {"fmt_desc_c", "C Source Code"},
      {"fmt_desc_cpp", "C++ Source Code"},
      {"fmt_desc_cc", "C++ Source Code"},
      {"fmt_desc_cxx", "C++ Source Code"},
      {"fmt_desc_h", "C/C++ Header File"},
      {"fmt_desc_hpp", "C++ Header File"},
      {"fmt_desc_hxx", "C++ Header File"},
      {"fmt_desc_py", "Python Script"},
      {"fmt_desc_java", "Java Source Code"},
      {"fmt_desc_go", "Go Source Code"},
      {"fmt_desc_rs", "Rust Source Code"},
      {"fmt_desc_js", "JavaScript Source"},
      {"fmt_desc_ts", "TypeScript Source"},
      {"fmt_desc_jsx", "React JSX Component"},
      {"fmt_desc_tsx", "React TSX Component"},
      {"fmt_desc_sh", "Shell Script"},
      {"fmt_desc_bat", "Windows Batch Script"},
      {"fmt_desc_cmd", "Windows Command Script"},
      {"fmt_desc_ps1", "PowerShell Script"},
      {"fmt_desc_sql", "SQL Database Script"},
      {"fmt_desc_php", "PHP Script"},
      {"fmt_desc_rb", "Ruby Script"},
      {"fmt_desc_swift", "Swift Source Code"},
      {"fmt_desc_kt", "Kotlin Source Code"},
      {"fmt_desc_scala", "Scala Source Code"},
      {"fmt_desc_lua", "Lua Script"},
      {"fmt_desc_r", "R Statistical Script"},
      {"fmt_desc_pl", "Perl Script"},
      {"fmt_desc_perl", "Perl Script"},
      {"fmt_desc_asm", "Assembly Source Code"},
      {"fmt_desc_asp", "Active Server Pages"},
      {"fmt_desc_aspx", "ASP.NET Web Page"},
      {"fmt_desc_bas", "BASIC Source Code"},
      {"fmt_desc_cmake", "CMake Build Script"},
      {"fmt_desc_cs", "C# Source Code"},
      {"fmt_desc_d", "D Source Code"},
      {"fmt_desc_f", "Fortran Source Code"},
      {"fmt_desc_fpp", "Fortran Preprocessed Source"},
      {"fmt_desc_fs", "F# Source Code"},
      {"fmt_desc_jsp", "Java Server Pages"},
      {"fmt_desc_pas", "Pascal Source Code"},
      {"fmt_desc_tcl", "Tcl/Tk Script"},
      {"fmt_desc_vb", "Visual Basic Source Code"},
      {"fmt_desc_vbs", "VBScript Script"},
      {"fmt_desc_ws", "Windows Script Host File"},
      {"fmt_desc_xml", "Extensible Markup Language"},
      {"fmt_desc_xsd", "XML Schema Definition"},
      {"fmt_desc_xsl", "Extensible Stylesheet Language"},
      {"fmt_desc_csv", "Comma-Separated Values"},
      {"fmt_desc_json", "JSON Data Object"},
      {"fmt_desc_yml", "YAML Configuration File"},
      {"fmt_desc_yaml", "YAML Configuration File"},
      {"fmt_desc_toml", "TOML Configuration File"},
      {"fmt_desc_ini", "Windows INI Configuration"},
      {"fmt_desc_conf", "Unix/Linux Configuration"},
      {"fmt_desc_cfg", "Application Configuration"},
      {"fmt_desc_rss", "RSS Feed Syndication"},
      {"fmt_desc_txt", "Plain Text Document"},
      {"fmt_desc_md", "Markdown Document"},
      {"fmt_desc_markdown", "Markdown Document"},
      {"fmt_desc_log", "Runtime Log File"},
      {"fmt_desc_zip", "ZIP Compressed Archive"},
      {"fmt_desc_tar", "Tape Archive Package"},
      {"fmt_desc_rar", "WinRAR Compressed Archive"},
      {"fmt_desc_7z", "7-Zip Compressed Archive"},
      {"fmt_desc_gz", "Gzip Compressed Archive"},
      {"fmt_desc_bz2", "Bzip2 Compressed Archive"},
      {"fmt_desc_xz", "XZ Compressed Archive"},
  };
}

}  // namespace gui
}  // namespace soeasy
