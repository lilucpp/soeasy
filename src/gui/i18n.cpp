#include "gui/i18n.h"

namespace soeasy {
namespace gui {

I18n &I18n::Instance() {
  static I18n instance;
  return instance;
}

I18n::I18n() {
  InitDictionary();
}

void I18n::SetLanguage(const std::string &lang_code) {
  if (lang_code == "en") {
    current_lang_ = Language::kEnglish;
  } else {
    current_lang_ = Language::kChinese;
  }
}

std::string I18n::GetLanguageCode() const {
  return current_lang_ == Language::kEnglish ? "en" : "zh";
}

QString I18n::Get(const char *key) const {
  if (!key) return QString();
  const auto &dict = (current_lang_ == Language::kEnglish) ? en_dict_ : zh_dict_;
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

QString I18n::GetCategoryName(size_t cat_index, const QString &default_name) const {
  std::string key = "cat_name_" + std::to_string(cat_index);
  QString val = Get(key.c_str());
  return val.isEmpty() ? default_name : val;
}

QString I18n::GetCategoryDesc(size_t cat_index, const QString &default_desc) const {
  std::string key = "cat_desc_" + std::to_string(cat_index);
  QString val = Get(key.c_str());
  return val.isEmpty() ? default_desc : val;
}

QString I18n::GetFormatDesc(const QString &ext, const QString &default_desc) const {
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
      {"app_name", "SoEasy"},
      {"app_slogan", "让电脑没有难搜的文件"},
      {"minimize", "最小化"},
      {"maximize", "最大化 / 还原"},
      {"close", "关闭"},
      {"browse", "浏览..."},
      {"msg_tip", "提示"},
      {"save_apply", "保存并应用"},
      {"cancel", "取消"},
      {"err_key_title", "SoEasy - 密钥错误"},
      {"err_key_msg", "获取本机数据库加密密钥失败:\n%1\n请检查设备权限或环境。"},
      {"err_startup_title", "SoEasy - 启动错误"},
      {"err_db_open_msg", "无法打开加密索引数据库:\n%1\n请检查权限或磁盘空间。"},
      {"err_pipeline_init_msg", "初始化索引流水线与文件监视引擎失败。"},

      // Search Bar
      {"search_placeholder", "输入搜索关键词（支持 FTS5 语法：AND, OR, NOT, \"精确词组\", 前缀*）..."},
      {"mode_fulltext", "全文倒排 (FTS5)"},
      {"mode_filename", "文件名秒搜"},
      {"btn_preview", "◨ 预览"},
      {"tip_preview", "切换右侧即时正文预览面板 (Ctrl+P)"},
      {"btn_settings", "⚙ 系统设置"},
      {"btn_scope_all", "📁 范围"},
      {"tip_scope_all", "限定搜索目录（留空为全盘搜索）"},
      {"tip_scope_active", "当前限定目录：%1\n点击更换，点击 ✕ 清除限定"},
      {"tip_clear_scope", "清除限定范围 (恢复全盘搜索)"},
      {"label_scope", "限定搜索路径:"},
      {"scope_placeholder", "留空表示全盘/全局检索；填入路径则仅在该目录及其子树下检索..."},
      {"btn_clear_scope", "清除范围"},
      {"dlg_select_scope", "选择限定搜索目录"},

      // Result Table
      {"th_id", "序号"},
      {"tip_header_id", "序号（结果列表展示位次）"},
      {"tip_reset_sort", "恢复默认排序"},
      {"th_filename", "文件名称"},
      {"th_path", "所在路径"},
      {"th_size", "大小"},
      {"th_mtime", "修改时间"},
      {"th_score", "相关度"},
      {"filename_match_prefix", "文件名匹配: %1"},

      // Preview Panel
      {"no_file_selected", "未选中文件"},
      {"btn_open_file", "打开文件"},
      {"btn_open_folder", "所在目录"},
      {"tip_close_preview", "收起预览面板 (Ctrl+P)"},
      {"keyword_label", "检索词:"},
      {"no_keywords", "全篇预览"},
      {"label_jump", "关键词跳转:"},
      {"search_in_content_ph", "在正文中查找..."},
      {"btn_prev", "▲ 上一个"},
      {"btn_next", "▼ 下一个"},
      {"match_count", "匹配: %1 / %2"},
      {"no_content_extracted", "无正文提取内容或尚未建立正文索引"},
      {"guide_title", "SoEasy 桌面搜索引擎"},
      {"guide_subtitle", "让电脑没有难搜的文件 · 快捷操作速查"},
      {"guide_kbd_title", "⌨️ 常用快捷键"},
      {"guide_tips_title", "💡 高效搜索技巧"},
      {"guide_kbd_nav", "在结果列表中切换并实时预览"},
      {"guide_kbd_open", "以默认程序直接打开文件"},
      {"guide_kbd_folder", "打开文件所在目录"},
      {"guide_kbd_focus", "快速聚焦至搜索框"},
      {"guide_kbd_preview", "显示 / 隐藏右侧预览面板"},
      {"guide_kbd_settings", "打开系统设置"},
      {"guide_kbd_esc", "清空搜索词 / 最小化至托盘"},
      {"guide_tips_html",
       "<li><b>全文检索 (FTS5)</b>：多词空格组合（如 <code>报告 2026</code>），毫秒级倒排索引</li>"
       "<li><b>文件名秒搜</b>：点击搜索栏左侧模式下拉可秒级查找任意文件名</li>"
       "<li><b>管理搜索目录</b>：点击右上角【⚙ 系统设置】可自由增减受索引监控的文件夹</li>"},

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
      {"status_badge_rebuild",
       "<span style=\"background-color:#FEF3C7; color:#B45309; font-weight:bold; padding:1px 6px; "
       "border-radius:3px;\">重建索引</span> "},
      {"status_badge_initial",
       "<span style=\"background-color:#E0F2FE; color:#0369A1; font-weight:bold; padding:1px 6px; "
       "border-radius:3px;\">首次索引</span> "},
      {"status_badge_incremental",
       "<span style=\"background-color:#DCFCE7; color:#15803D; font-weight:bold; padding:1px 6px; "
       "border-radius:3px;\">增量索引</span> "},
      {"status_rebuild_scanning", "正在清空旧缓存并扫描目录..."},
      {"status_scanning_dirs", "正在扫描待索引目录..."},
      {"status_init_manifest", "正在初始化文件清单..."},
      {"status_state_rebuilding", "<span style=\"color:#D97706;\">●</span> 重建中"},
      {"status_state_initializing", "<span style=\"color:#0284C7;\">●</span> 初始化中"},
      {"status_state_syncing", "<span style=\"color:#0D9488;\">●</span> 同步中"},
      {"status_building_progress", "正在构建: %1/%2 (%3%)"},
      {"status_parsing_file", "正在解析: %1"},
      {"status_task_cancelled_msg",
       "<span style=\"color:#D97706; font-weight:bold;\">! 索引任务已取消</span> 已保留已解析的数据"},
      {"status_finish_rebuild",
       "<span style=\"color:#16A34A; font-weight:bold;\">✓ 索引重建完成</span> 共重构 %1 个文件索引 (耗时 %2)"},
      {"status_finish_initial",
       "<span style=\"color:#16A34A; font-weight:bold;\">✓ 首次索引完成</span> 共索引 %1 个文件 (耗时 %2)"},
      {"status_finish_incremental",
       "<span style=\"color:#16A34A; font-weight:bold;\">✓ 增量同步完成</span> 索引已是最新 (耗时 %1)"},
      {"status_latency_rebuild", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> 重建耗时: %1"},
      {"status_latency_initial", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> 首次索引: %1"},
      {"status_latency_incremental", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> 增量同步: %1"},
      {"status_latency_cancelled", "<span style=\"color: #D97706; font-size: 13px;\">●</span> 索引已取消 (%1)"},
      {"status_latency_with_index", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> 索引: %1 | 检索: %2 ms"},
      {"status_index_tooltip", "最近索引任务: %1 (%2)\n处理文件: %3 个\n索引耗时: %4\n完成时间: %5"},
      {"status_search_tooltip_line", "\n最近检索耗时: %1 ms"},
      {"status_task_name_rebuild", "重建索引"},
      {"status_task_name_initial", "首次索引"},
      {"status_task_name_incremental", "增量同步"},
      {"status_task_state_done", "已完成"},
      {"status_task_state_cancelled", "已取消"},

      // Tray Icon
      {"tray_show", "显示主界面"},
      {"tray_settings", "系统设置..."},
      {"tray_quit", "退出程序"},
      {"tray_tip_msg", "SoEasy 已最小化到托盘，后台持续保持文件监控与索引。"},

      // Settings Dialog
      {"settings_title", "系统设置"},
      {"tab_general", "常规与语言"},
      {"tab_dirs", "索引目录"},
      {"tab_patterns", "排除目录规则"},
      {"tab_exts", "文件格式与解析"},
      {"tab_user_dict", "分词词库"},
      {"tab_perf", "性能与日志"},

      // Settings - User Dict Tab
      {"label_user_dict_hint", "自定义专有关键词，检索时具备即时自愈能力。点击“重建索引”可全量重新分词："},
      {"col_user_dict_word", "自定义关键词"},
      {"col_user_dict_freq", "词频权重"},
      {"btn_add_word", "添加关键词"},
      {"btn_rm_word", "移除所选"},
      {"word_ph", "输入新关键词，回车或点击添加..."},
      {"label_word_count", "已加载自定义关键词数: %1"},
      {"tip_word_exists", "该关键词已存在！"},
      {"tip_word_exists_in_custom", "该关键词已在自定义列表中！"},
      {"tip_word_exists_in_system", "该关键词已存在于系统基础词库中，分词器天然支持精准切词，无需重复添加。"},

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
      {"tip_embedded_ocr",
       "💡 默认不勾选。勾选后将提取 Office/PDF/ODF 等文档中的嵌入插图并调用 PP-OCRv6 引擎进行文本识别融合；\n    "
       "不勾选可获得极致解析性能（纯文本毫秒级完成索引）。"},
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
      {"btn_save", "保存并应用"},
      {"btn_cancel", "取消"},
      {"btn_browse", "浏览..."},
      {"dlg_error", "错误"},
      {"dlg_tip", "提示"},
      {"save_cfg_failed", "保存配置文件失败，请检查权限或磁盘空间。"},
      {"rebuild_confirm_title", "确认重建索引"},
      {"rebuild_confirm_text",
       "此操作将彻底清空当前所有的全文倒排索引与密态缓存，\n并对所有配置的监控目录从头进行全量重新解析与构建。\n\n确定"
       "要继续吗？"},
      {"rebuild_confirm_msg",
       "此操作将彻底清空当前所有的全文倒排索引与密态缓存，\n并对所有配置的监控目录从头进行全量重新解析与构建。\n\n确定"
       "要继续吗？"},

      // Settings Tabs (refactored key aliases)
      {"label_ui_language", "界面显示语言 (UI Language):"},
      {"dlg_choose_dir", "选择待索引的目录"},
      {"word_exists_dict", "关键词“%1”已在自定义列表中！"},
      {"word_exists_base", "关键词“%1”已存在于系统基础词库中，分词器天然支持精准切词，无需重复添加。"},
      {"ocr_group_title", "高级解析能力"},
      {"ocr_checkbox_label", "提取并 OCR 解析文档内嵌图片 (Office 系列 / PDF / ODF 等)"},
      {"ocr_tip_label",
       "💡 默认不勾选。勾选后将提取 Office/PDF/ODF 等文档中的嵌入插图并调用 PP-OCRv6 引擎进行文本识别融合；\n    "
       "不勾选可获得极致解析性能（纯文本毫秒级完成索引）。"},
      {"exts_tree_title", "支持与索引的文件格式分类（勾选以启用全文索引）："},
      {"tree_col_category", "分类 / 格式扩展名"},
      {"tree_col_desc", "说明与处理引擎"},
      {"cat_custom_title", "用户自定义格式 (Custom)"},
      {"cat_custom_desc", "用户手动添加的专有文件后缀与提取规则"},
      {"label_custom_ext", "添加自定义扩展名:"},
      {"btn_add_ext", "添加格式"},
      {"btn_rm_custom_ext", "移除所选自定义"},
      {"label_max_size", "单文件最大全文索引上限:"},
      {"filter_thumbs_check", "自动忽略应用缓存缩略图与图标 (如 WeAppIcon, Thumb)"},
      {"label_console_out", "终端输出调试:"},
      {"chk_console_out", "输出控制台实时镜像 (Console stdout，用于终端调试)"},
      {"label_sync_flush", "落盘同步模式:"},
      {"chk_sync_flush", "同步即时落盘模式 (Sync Flush，每条日志即刻刷盘，排查异常卡死)"},
      {"chk_dt_log", "启用 DocToText 详细文档解析日志 (默认关闭，用于排查 Office/PDF 解析问题)"},
      {"dt_log_ph", "留空则默认输出至 logs/doctotext.log"},
      {"dlg_choose_log_file", "选择 DocToText 日志输出文件"},

      // Content Preview Dialog
      {"preview_dlg_title", "文档正文预览 - %1"},
      {"preview_loading_content", "正在加载正文预览..."},
      {"preview_cannot_read", "无法读取文档内容或正文为空。"},
      {"preview_content_truncated", "……（正文过长，预览已截断）"},
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
      {"app_name", "SoEasy"},
      {"app_slogan", "Make Every File Searchable"},
      {"minimize", "Minimize"},
      {"maximize", "Maximize / Restore"},
      {"close", "Close"},
      {"browse", "Browse..."},
      {"msg_tip", "Notice"},
      {"save_apply", "Save && Apply"},
      {"cancel", "Cancel"},
      {"err_key_title", "SoEasy - Key Error"},
      {"err_key_msg",
       "Failed to retrieve local database encryption key:\n%1\nPlease check device permissions or environment."},
      {"err_startup_title", "SoEasy - Startup Error"},
      {"err_db_open_msg", "Failed to open encrypted index database:\n%1\nPlease check permissions or disk space."},
      {"err_pipeline_init_msg", "Failed to initialize index pipeline and file watching engine."},

      // Search Bar
      {"search_placeholder", "Enter search keywords (Supports FTS5: AND, OR, NOT, \"exact phrase\", prefix*)..."},
      {"mode_fulltext", "Full-Text (FTS5)"},
      {"mode_filename", "Filename Search"},
      {"btn_preview", "◨ Preview"},
      {"tip_preview", "Toggle preview pane (Ctrl+P)"},
      {"btn_settings", "⚙ System Settings"},
      {"btn_scope_all", "📁 Scope"},
      {"tip_scope_all", "Constrain search directory (empty for all)"},
      {"tip_scope_active", "Current scope: %1\nClick to change, click ✕ to clear"},
      {"tip_clear_scope", "Clear scope (restore global search)"},
      {"label_scope", "Search Scope:"},
      {"scope_placeholder", "Leave empty for global search; or enter path to search within directory tree..."},
      {"btn_clear_scope", "Clear Scope"},
      {"dlg_select_scope", "Select Directory for Search Scope"},

      // Result Table
      {"th_id", "#"},
      {"tip_header_id", "# (Row Index)"},
      {"tip_reset_sort", "Restore default order"},
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
      {"tip_close_preview", "Collapse preview pane (Ctrl+P)"},
      {"keyword_label", "Keywords:"},
      {"no_keywords", "Full Preview"},
      {"label_jump", "Jump to Match:"},
      {"search_in_content_ph", "Find in content..."},
      {"btn_prev", "▲ Previous"},
      {"btn_next", "▼ Next"},
      {"match_count", "Match: %1 / %2"},
      {"no_content_extracted", "No text extracted or document not yet indexed"},
      {"guide_title", "SoEasy Desktop Search"},
      {"guide_subtitle", "Fast, local & secure desktop search · Quick Reference"},
      {"guide_kbd_title", "⌨️ Keyboard Shortcuts"},
      {"guide_tips_title", "💡 Search Tips"},
      {"guide_kbd_nav", "Navigate results with live preview"},
      {"guide_kbd_open", "Open file with default program"},
      {"guide_kbd_folder", "Open containing folder"},
      {"guide_kbd_focus", "Focus search input box"},
      {"guide_kbd_preview", "Toggle right preview panel"},
      {"guide_kbd_settings", "Open System Settings"},
      {"guide_kbd_esc", "Clear query / hide to tray"},
      {"guide_tips_html",
       "<li><b>Full-text Search (FTS5)</b>: Multi-word space combinations (e.g. <code>report 2026</code>)</li>"
       "<li><b>Instant Filename</b>: Toggle mode to instant filename matching across all files</li>"
       "<li><b>Manage Directories</b>: Click [⚙ System Settings] to configure monitored folders</li>"},

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
      {"status_badge_rebuild",
       "<span style=\"background-color:#FEF3C7; color:#B45309; font-weight:bold; padding:1px 6px; "
       "border-radius:3px;\">Rebuild</span> "},
      {"status_badge_initial",
       "<span style=\"background-color:#E0F2FE; color:#0369A1; font-weight:bold; padding:1px 6px; "
       "border-radius:3px;\">Initial Index</span> "},
      {"status_badge_incremental",
       "<span style=\"background-color:#DCFCE7; color:#15803D; font-weight:bold; padding:1px 6px; "
       "border-radius:3px;\">Incremental</span> "},
      {"status_rebuild_scanning", "Clearing old cache and scanning directories..."},
      {"status_scanning_dirs", "Scanning watched directories..."},
      {"status_init_manifest", "Initializing file manifest..."},
      {"status_state_rebuilding", "<span style=\"color:#D97706;\">●</span> Rebuilding"},
      {"status_state_initializing", "<span style=\"color:#0284C7;\">●</span> Initializing"},
      {"status_state_syncing", "<span style=\"color:#0D9488;\">●</span> Syncing"},
      {"status_building_progress", "Building: %1/%2 (%3%)"},
      {"status_parsing_file", "Parsing: %1"},
      {"status_task_cancelled_msg",
       "<span style=\"color:#D97706; font-weight:bold;\">! Indexing Cancelled</span> Parsed data preserved"},
      {"status_finish_rebuild",
       "<span style=\"color:#16A34A; font-weight:bold;\">✓ Rebuild Complete</span> %1 files indexed (%2)"},
      {"status_finish_initial",
       "<span style=\"color:#16A34A; font-weight:bold;\">✓ Initial Index Complete</span> %1 files indexed (%2)"},
      {"status_finish_incremental",
       "<span style=\"color:#16A34A; font-weight:bold;\">✓ Incremental Sync Complete</span> Up to date (%1)"},
      {"status_latency_rebuild", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> Rebuild: %1"},
      {"status_latency_initial", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> Initial index: %1"},
      {"status_latency_incremental", "<span style=\"color: #16A34A; font-size: 13px;\">●</span> Sync: %1"},
      {"status_latency_cancelled", "<span style=\"color: #D97706; font-size: 13px;\">●</span> Cancelled (%1)"},
      {"status_latency_with_index",
       "<span style=\"color: #16A34A; font-size: 13px;\">●</span> Index: %1 | Search: %2 ms"},
      {"status_index_tooltip", "Last index task: %1 (%2)\nIndexed files: %3\nElapsed: %4\nFinished at: %5"},
      {"status_search_tooltip_line", "\nLast search: %1 ms"},
      {"status_task_name_rebuild", "Rebuild Index"},
      {"status_task_name_initial", "Initial Index"},
      {"status_task_name_incremental", "Incremental Sync"},
      {"status_task_state_done", "Completed"},
      {"status_task_state_cancelled", "Cancelled"},

      // Tray Icon
      {"tray_show", "Show MainWindow"},
      {"tray_settings", "System Settings..."},
      {"tray_quit", "Quit SoEasy"},
      {"tray_tip_msg", "SoEasy is minimized to system tray and watching files in background."},

      // Settings Dialog
      {"settings_title", "System Settings"},
      {"tab_general", "General && Language"},
      {"tab_dirs", "Indexed Directories"},
      {"tab_patterns", "Excluded Patterns"},
      {"tab_exts", "File Formats"},
      {"tab_user_dict", "User Dictionary"},
      {"tab_perf", "Performance && Logs"},

      // Settings - User Dict Tab
      {"label_user_dict_hint",
       "Custom keywords with instant search self-healing. Click 'Rebuild Index' to re-index historical files:"},
      {"col_user_dict_word", "Custom Keyword"},
      {"col_user_dict_freq", "Weight"},
      {"btn_add_word", "Add Keyword"},
      {"btn_rm_word", "Remove Selected"},
      {"word_ph", "Enter new keyword, press Enter or click Add..."},
      {"label_word_count", "Total custom keywords: %1"},
      {"tip_word_exists", "Keyword already exists!"},
      {"tip_word_exists_in_custom", "Keyword already exists in the custom list!"},
      {"tip_word_exists_in_system",
       "Keyword already exists in system base dictionary; tokenization is naturally supported."},

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
      {"tip_embedded_ocr",
       "💡 Unchecked by default. Extracts embedded images for PP-OCRv6 recognition.\n    Unchecked yields maximum "
       "indexing throughput (instant plain-text extraction)."},
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
      {"btn_save", "Save && Apply"},
      {"btn_cancel", "Cancel"},
      {"btn_browse", "Browse..."},
      {"dlg_error", "Error"},
      {"dlg_tip", "Information"},
      {"save_cfg_failed", "Failed to save configuration file. Please check permissions or disk space."},
      {"rebuild_confirm_title", "Confirm Index Rebuild"},
      {"rebuild_confirm_text",
       "This will clear all inverted indices and encrypted caches,\nand completely rebuild indices from scratch for "
       "all configured directories.\n\nAre you sure you want to proceed?"},
      {"rebuild_confirm_msg",
       "This will clear all inverted indices and encrypted caches,\nand completely rebuild indices from scratch for "
       "all configured directories.\n\nAre you sure you want to proceed?"},

      // Settings Tabs (refactored key aliases)
      {"label_ui_language", "Interface Language (UI Language):"},
      {"dlg_choose_dir", "Select Directory to Index"},
      {"word_exists_dict", "Keyword '%1' already exists in the custom list!"},
      {"word_exists_base",
       "Keyword '%1' already exists in system base dictionary; tokenization is naturally supported."},
      {"ocr_group_title", "Advanced Parsing Capabilities"},
      {"ocr_checkbox_label", "Extract && OCR embedded images in documents (Office / PDF / ODF)"},
      {"ocr_tip_label",
       "💡 Unchecked by default. Extracts embedded images for PP-OCRv6 recognition.\n    Unchecked yields maximum "
       "indexing throughput (instant plain-text extraction)."},
      {"exts_tree_title", "Supported file format categories (check to enable full-text indexing):"},
      {"tree_col_category", "Category / Extension"},
      {"tree_col_desc", "Description & Parser Engine"},
      {"cat_custom_title", "User-Defined Formats (Custom)"},
      {"cat_custom_desc", "Manually registered file extensions and extraction rules"},
      {"label_custom_ext", "Add Custom Extension:"},
      {"btn_add_ext", "Add Format"},
      {"btn_rm_custom_ext", "Remove Selected"},
      {"label_max_size", "Max indexable file size:"},
      {"filter_thumbs_check", "Filter out cache thumbnails & icons (e.g. WeAppIcon, Thumb)"},
      {"label_console_out", "Console output:"},
      {"chk_console_out", "Mirror logs to console stdout (terminal debugging)"},
      {"label_sync_flush", "Disk sync mode:"},
      {"chk_sync_flush", "Synchronous flush mode (immediate disk sync for crash diagnosis)"},
      {"chk_dt_log", "Enable DocToText verbose logging (troubleshooting Office/PDF parsing)"},
      {"dt_log_ph", "Leave empty for default logs/doctotext.log"},
      {"dlg_choose_log_file", "Select DocToText Log File"},

      // Content Preview Dialog
      {"preview_dlg_title", "Content Preview - %1"},
      {"preview_loading_content", "Loading content preview..."},
      {"preview_cannot_read", "Unable to read document content or content is empty."},
      {"preview_content_truncated", "... (content too large, preview truncated)"},
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
