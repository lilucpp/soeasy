// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#ifndef SOEASY_SRC_COMMON_FORMAT_CATEGORIES_H_
#define SOEASY_SRC_COMMON_FORMAT_CATEGORIES_H_

#include <string>
#include <unordered_set>
#include <vector>

namespace soeasy {
namespace common {

struct FormatDef {
  const char* ext;
  const char* desc;
  bool default_enabled;
  bool disabled = false;
};

struct CategoryDef {
  const char* name;
  const char* desc;
  std::vector<FormatDef> formats;
  bool disabled = false;
};

inline const std::vector<CategoryDef>& GetPresetCategories() {
  static const std::vector<CategoryDef> kCategories = {
    {
      "Microsoft Office 新版格式 (OOXML)",
      "基于 OpenXML 标准的现代化办公文档",
      {
        {"docx", "Word 2007+ 文本文档", true},
        {"xlsx", "Excel 2007+ 电子表格", true},
        {"pptx", "PowerPoint 2007+ 演示文稿", true},
      }
    },
    {
      "Microsoft Office 经典二进制格式",
      "Microsoft Office 97-2003 经典二进制格式",
      {
        {"doc", "Word 97-2003 文档", true},
        {"xls", "Excel 97-2003 工作簿", true},
        {"ppt", "PowerPoint 97-2003 演示文稿", true},
        {"xlsb", "Excel 二进制工作簿", false},
      }
    },
    {
      "开放文档格式 (ODF / LibreOffice)",
      "OASIS OpenDocument 开放文档标准",
      {
        {"odt", "OpenDocument 文本文档", true},
        {"ods", "OpenDocument 电子表格", true},
        {"odp", "OpenDocument 演示文稿", true},
        {"odg", "OpenDocument 绘图文档", true},
        {"fodt", "Flat XML ODF 文本文档", true},
        {"fods", "Flat XML ODF 电子表格", true},
        {"fodp", "Flat XML ODF 演示文稿", true},
      }
    },
    {
      "便携式文档 (PDF)",
      "Adobe PDF 电子出版物与矢量文档",
      {
        {"pdf", "Adobe PDF 便携式文档", true},
      }
    },
    {
      "网页与富文本文档",
      "超文本网页与跨平台富文本格式",
      {
        {"html", "HTML 超文本标记语言", true},
        {"htm", "HTM 网页文件", true},
        {"css", "层叠样式表 (CSS)", true},
        {"rtf", "Rich Text Format 富文本", true},
      }
    },
    {
      "电子邮件格式 (Email)",
      "MIME 规范电子邮件及附件",
      {
        {"eml", "RFC 822 电子邮件消息与附件", false},
      }
    },
    {
      "独立图像格式 (PP-OCRv6 图像文字识别)",
      "包含深度学习光学字符识别能力的图像文件",
      {
        {"jpg", "JPEG 标准图像", true},
        {"jpeg", "JPEG 标准图像", true},
        {"png", "PNG 便携式网络图形", true},
        {"webp", "Google WebP 高效图像", true},
        {"bmp", "Windows 位图", true},
        {"tiff", "TIFF 标签图像文件", true},
        {"tif", "TIF 图像文件", true},
        {"jfif", "JPEG 文件交换格式", true},
        {"pnm", "便携式任意图文件", true},
      }
    },
    {
      "苹果办公套件 (Apple iWork)",
      "Apple macOS/iOS 办公套件文档",
      {
        {"pages", "Apple Pages 文本文档", true},
        {"numbers", "Apple Numbers 电子表格", true},
        {"key", "Apple Keynote 幻灯片", true},
        {"keynote", "Apple Keynote 演示文稿", true},
      }
    },
    {
      "编程语言与脚本源代码",
      "主流编程语言源文件与系统运维脚本",
      {
        {"c", "C 语言源代码", false},
        {"cpp", "C++ 语言源代码", false},
        {"cc", "C++ 语言源文件", false},
        {"cxx", "C++ 语言源文件", false},
        {"h", "C/C++ 头文件", false},
        {"hpp", "C++ 头文件", false},
        {"hxx", "C++ 头文件", false},
        {"py", "Python 脚本", false},
        {"java", "Java 源代码", false},
        {"go", "Go 语言源文件", false},
        {"rs", "Rust 源代码", false},
        {"js", "JavaScript 脚本", false},
        {"ts", "TypeScript 脚本", false},
        {"jsx", "React JSX 组件", false},
        {"tsx", "React TSX 组件", false},
        {"sh", "Shell 脚本", false},
        {"bat", "Windows 批处理脚本", false},
        {"cmd", "Windows 命令脚本", false},
        {"ps1", "PowerShell 脚本", false},
        {"sql", "SQL 数据库脚本", false},
        {"php", "PHP 脚本", false},
        {"rb", "Ruby 脚本", false},
        {"swift", "Swift 源代码", false},
        {"kt", "Kotlin 源代码", false},
        {"scala", "Scala 源代码", false},
        {"lua", "Lua 脚本", false},
        {"r", "R 统计语言脚本", false},
        {"pl", "Perl 脚本", false},
        {"perl", "Perl 脚本", false},
        {"asm", "汇编语言源程序", false},
        {"asp", "Active Server Pages", false},
        {"aspx", "ASP.NET 网页程序", false},
        {"bas", "BASIC 源代码", false},
        {"cmake", "CMake 构建脚本", false},
        {"cs", "C# 语言源代码", false},
        {"d", "D 语言源代码", false},
        {"f", "Fortran 源代码", false},
        {"fpp", "Fortran 预处理源码", false},
        {"fs", "F# 源代码", false},
        {"jsp", "Java Server Pages", false},
        {"pas", "Pascal 源代码", false},
        {"tcl", "Tcl/Tk 脚本", false},
        {"vb", "Visual Basic 源代码", false},
        {"vbs", "VBScript 脚本", false},
        {"ws", "Windows 脚本宿主文件", false},
      }
    },
    {
      "XML 标记语言家族",
      "XML 格式及其模式与转换定义",
      {
        {"xml", "XML 可扩展标记语言", true},
        {"xsd", "XML 架构定义文件", false},
        {"xsl", "可扩展样式表转换", false},
      }
    },
    {
      "逗号分隔值表格 (CSV)",
      "轻量纯文本结构化表格数据",
      {
        {"csv", "Comma-Separated Values", true},
      }
    },
    {
      "结构化数据与配置文件",
      "常见系统配置、应用参数与结构化描述文件",
      {
        {"json", "JSON 对象描述数据", false},
        {"yml", "YAML 结构化配置文件", false},
        {"yaml", "YAML 结构化配置文件", false},
        {"toml", "TOML 现代配置文件", false},
        {"ini", "Windows INI 配置", false},
        {"conf", "Unix/Linux 配置文件", false},
        {"cfg", "应用程序配置文件", false},
        {"rss", "RSS 联合信息推送文件", false},
      }
    },
    {
      "纯文本与日志文件",
      "通用纯文本文档与运行日志记录",
      {
        {"txt", "普通纯文本文档", true},
        {"md", "Markdown 轻量标记文档", true},
        {"markdown", "Markdown 文档", true},
        {"log", "运行日志记录文件", false},
      }
    },
    {
      "压缩归档格式 (Archives)",
      "打包与压缩文件容器 (正文递归检索规划中)",
      {
        {"zip", "ZIP 压缩文件", false, true},
        {"tar", "Tape Archive 归档包", false, true},
        {"rar", "WinRAR 压缩包", false, true},
        {"7z", "7-Zip 压缩包", false, true},
        {"gz", "Gzip 压缩文件", false, true},
        {"bz2", "Bzip2 压缩文件", false, true},
        {"xz", "XZ 压缩文件", false, true},
      },
      true  // 暂未支持，界面默认 disable，后续支持后直接设为 false 即可启用
    }
  };
  return kCategories;
}

inline std::unordered_set<std::string> GetDefaultSupportedExtensions() {
  std::unordered_set<std::string> defaults;
  for (const auto& cat : GetPresetCategories()) {
    if (cat.disabled) continue;
    for (const auto& fmt : cat.formats) {
      if (fmt.default_enabled && !fmt.disabled) {
        defaults.insert(fmt.ext);
      }
    }
  }
  return defaults;
}

}  // namespace common
}  // namespace soeasy

#endif  // SOEASY_SRC_COMMON_FORMAT_CATEGORIES_H_
