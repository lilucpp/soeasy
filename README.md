# SoEasy (Sou Easy)

基于 C++17 与 Qt5 的跨平台桌面全文检索与文件管理工具。

## 功能特性

- **全文检索与即时高亮**：基于 SQLite FTS5 与 cppjieba 中文分词，支持正文关键词检索与上下文高亮摘要。
- **多格式解析支持**：
  - 纯文本与源码：`.txt`, `.md`, `.log`, `.csv`, `.json`, `.xml`, `.html` 及常见编程语言源码。
  - 办公与版式文档：`.doc`, `.docx`, `.xls`, `.xlsx`, `.ppt`, `.pptx`, `.pdf`。
- **多进程解析沙箱**：解析器与主服务进程物理隔离，支持超时强杀与异常自愈，防止复杂或损坏文档影响主程序稳定性。
- **本地密态存储**：基于 SQLite3MultipleCiphers (AES-128) 对索引库进行透明页级加密，文档全文采用 Zstandard (Zstd) 压缩存储。
- **实时监控与增量索引**：监听目录变动事件并进行去抖合并，自动维护增量索引。
- **双栏预览交互**：提供搜索结果列表与右侧即时正文预览（Master-Detail 布局），支持文档内关键词匹配跳转与定位。

## 架构概览

```mermaid
flowchart TD
    subgraph UI ["前端界面 (Qt5)"]
        MainWindow["主窗口 (无边框 / 双栏预览)"]
        ResultView["搜索列表 (视口懒加载)"]
        PreviewPanel["正文预览与关键词高亮"]
    end

    subgraph Service ["核心服务 (soeasy-service)"]
        IndexPipeline["索引流水线 / 规则中心"]
        WatchEngine["目录变动监控 (efsw)"]
        WorkerGuard["沙箱守卫 (进程管理 / IPC)"]
    end

    subgraph Storage ["存储引擎 (SQLite3MC + Zstd)"]
        FTS5["FTS5 全文倒排表 (cppjieba)"]
        DocContents["正文压缩存储 (Zstd Level 3)"]
    end

    subgraph Workers ["解析子进程 (soeasy-worker)"]
        L1["文本与代码解析"]
        L2["办公文档与 PDF 抽取"]
    end

    MainWindow --> IndexPipeline
    IndexPipeline --> WatchEngine
    IndexPipeline --> WorkerGuard
    WorkerGuard --> Workers
    IndexPipeline --> Storage
    ResultView --> Storage
    PreviewPanel --> Storage
```

## 编译与运行

### 依赖环境

- **编译器**：支持 C++17 的 GCC 8+、Clang 9+ 或 MSVC 2019+
- **CMake**：3.16 及以上
- **依赖库**：Qt5 (Widgets, Gui, Core)、Cap'n Proto、uchardet、DocToText、zlib、libxml2

### 编译步骤

```bash
# 生成构建目录
cmake -B build -DCMAKE_BUILD_TYPE=Release

# 编译项目
cmake --build build -j4

# 运行 GUI
./build/soeasy-gui
```

### 运行测试

```bash
export LD_LIBRARY_PATH=./third_party/sysroot/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

# 存储引擎基准测试
./build/test_storage_engine

# GUI 前端集成验证
./build/test_gui_smoke
```

### 打包分发 (Linux)

```bash
bash scripts/package.sh
```
打包输出产物位于 `dist/soeasy-v0.1.0-linux-x86_64.zip`。

---

## Windows (x64) 交叉编译与打包 (基于 Docker)

本项目支持在 Linux 宿主机上通过 Docker 容器化 MinGW-w64 工具链一键交叉编译并打包 Windows 64位桌面应用（包含完整的 Qt5、OpenCV、PP-OCRv6 与 DocToText 运行时）。

### 1. 制作交叉编译镜像

镜像基于 `Fedora 40`，集成了官方 MinGW-w64 预编译开发套件（GCC 14、Qt 5.15、OpenCV 4.7）及静态 Cap'n Proto 和 uchardet 库：

```bash
docker build -f Dockerfile.win64 -t soeasy-win64-cross .
```

### 2. 启动容器编译项目

挂载项目目录并指定实例名 `--name soeasy-win64-builder`，调用 `mingw64-cmake` 进行跨平台构建：

```bash
# 生成 CMake 构建配置 (Windows x64 Release)
docker run --name soeasy-win64-builder --rm -v "$PWD":/workspace -w /workspace soeasy-win64-cross \
  mingw64-cmake -B build-win64 -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF

# 编译生成 soeasy-gui.exe 与 soeasy-worker.exe
docker run --name soeasy-win64-builder --rm -v "$PWD":/workspace -w /workspace soeasy-win64-cross \
  ninja -C build-win64 soeasy-gui soeasy-worker -j$(nproc)
```

### 3. 一键打包 Windows 发行版

项目提供了自动化打包脚本，会自动检测镜像、执行编译并从容器 sysroot 中提取所有必需的 MinGW DLL（Qt5 核心与平台插件、OpenCV、Uchardet、Iconv 等），打包为即开即用的 zip 发行包：

```bash
./scripts/package_win64_docker.sh
```

打包产物位于 `dist/soeasy-v0.1.0-windows-x64.zip`，解压至任意 Windows 64位机器即可直接运行 `bin/soeasy-gui.exe`。

## 目录结构

```text
soeasy/
├── CMakeLists.txt          # CMake 构建配置
├── README.md               # 项目说明文档
├── schema/                 # 进程间通信协议 (Cap'n Proto)
├── scripts/                # 打包与发布脚本
├── src/
│   ├── common/             # 共享内存、日志工具
│   ├── gui/                # Qt5 界面组件 (主窗口、搜索栏、列表、预览)
│   ├── service/            # 索引流水线、目录监控、沙箱守卫
│   ├── storage/            # 存储引擎、FTS5 结巴分词器
│   └── worker/             # 解析子进程及各格式抽取器
├── tests/                  # 自动化测试用例
└── third_party/            # 第三方依赖与头文件
```

## 许可证

本项目基于 [MIT License](LICENSE) 开源。
