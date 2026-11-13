# SoEasy

> **Make Every File Searchable On Your Desktop**  
> *让电脑没有难搜的文件*

[English](README.md) | [简体中文](README_zh.md) | [Release v0.3.0](https://github.com/lilucpp/soeasy/releases/tag/v0.3.0)

---

SoEasy is a native, privacy-first, ultra-fast desktop search engine. Built with modern C++17 and Qt5, it indexes and searches source code repositories, Office suites, spreadsheets, presentations, PDFs, and scanned documents entirely offline with sub-millisecond query response and instant content preview.

Zero Electron bloat, zero cloud telemetry, and complete crash resilience through multi-process sandbox isolation.

<p align="center">
  <img src="docs/screenshot.png" alt="SoEasy Desktop Search Interface" width="880">
</p>

---

## Highlights

- **Instant Full-Text & Filename Search**: Powered by SQLite FTS5 with Double-Array Trie Jieba tokenization and BM25 ranking. Features live debounced in-document keyword preview.
- **Crash-Resilient Sandbox Isolation**: All document parsing runs within an isolated subprocess (`soeasy-worker`) governed by modern `reproc++`. Malformed or looping files are killed and self-healed in milliseconds without freezing the UI.
- **Double-Array Trie (DAT) + mmap Tokenization**: Precompiles 350,000 dictionary words into contiguous DAT binary blocks, mapped via kernel `mmap`. Memory footprint plunges from **120 MB down to 7.2 MB** (94% RAM reduction).
- **Lightweight Deep Learning OCR**: Bundles PP-OCRv6 with zero-allocation Header Probe nanosecond sniffing ($< 1\,\mu\text{s}$) to filter micro-icons and borders before decoding. Cuts RAM by 34% and accelerates inference by >40%.
- **Hardware-Bound Local Encryption**: Zero cloud dependence. Index databases are protected with page-level AES-128 encryption (SQLite3MultipleCiphers) and Zstandard Level 3 compression, bound to host hardware to prevent offline theft.
- **Modern Component Architecture**: Features composite filename and path presentation, a clean single-line search bar, full keyboard navigation (<kbd>Enter</kbd>, <kbd>Ctrl+O</kbd>, <kbd>Ctrl+P</kbd>, <kbd>F3</kbd>), and real-time incremental filesystem watching.

> 💡 **Deep Architectural Specifications**: For in-depth implementation details on process supervision, DAT Trie derivation, nanosecond Header Probe heuristics, and machine-bound envelope encryption, see 📖 **[Deep Architectural Principles (docs/features.md)](docs/features.md)**.

---

## Architecture Overview

```mermaid
flowchart TD
    subgraph UI ["Front-End (Qt5 / ThemeManager)"]
        MainWindow["Main Window (Single-Line Bar & Dual-Pane)"]
        ResultView["Search Table (Virtual Viewport & Composite Rows)"]
        PreviewPanel["Live Preview (Async ThreadPool Debounce)"]
        DocViewer["DocumentViewerWidget (Highlight & Traversal)"]
    end

    subgraph Core ["Core Service (soeasy-service)"]
        IndexPipeline["Index Pipeline & Rules Engine"]
        WatchEngine["Filesystem Watcher (efsw)"]
        WorkerGuard["Sandbox Guard / Watchdog (reproc++)"]
        MachineKey["MachineKeyProvider (Hardware Envelope Encryption)"]
    end

    subgraph Sandbox ["Parser Sandbox (soeasy-worker)"]
        L1["L1: Plain Text & Code (uchardet)"]
        L2["L2: Office / PDF / DocToText Enhanced"]
        L3["L3: Images & Scans (PP-OCRv6)"]
    end

    subgraph Storage ["Encrypted Storage (SQLite3MC + Zstd)"]
        FTS5["Inverted Index (DAT Tokenizer + BM25)"]
        CompressedCache["Text Cache (Zstd Level 3)"]
    end

    MainWindow --> IndexPipeline
    PreviewPanel --> DocViewer
    IndexPipeline --> WatchEngine
    IndexPipeline --> WorkerGuard
    WorkerGuard -- Shared Memory / IPC --> Sandbox
    MachineKey -.-> Storage
    IndexPipeline --> Storage
    ResultView --> Storage
    PreviewPanel -.-> Storage
```

---

## Supported Formats

| Category | File Extensions | Extraction Engine |
| :--- | :--- | :--- |
| **Plain Text & Source Code** | `.txt`, `.md`, `.log`, `.csv`, `.json`, `.xml`, `.html`, `.c`, `.cpp`, `.py`, `.go`, `.rs`, `.java`, `.ts`, `.sh`, etc. | L1 Native parser + uchardet auto-sniffing |
| **Microsoft Office (OOXML)** | `.docx`, `.xlsx`, `.pptx` | L2 DocToText / Fast stream extraction |
| **Microsoft Office Binary** | `.doc`, `.xls`, `.ppt`, `.xlsb` | L2 DocToText binary extractors |
| **OpenDocument (ODF)** | `.odt`, `.ods`, `.odp`, `.odg`, `.fodt`, `.fods`, `.fodp` | L2 DocToText |
| **Portable Documents** | `.pdf` (text layer + automatic scan queueing) | L2 Poppler natural flow + OCR fallback |
| **Apple iWork** | `.pages`, `.numbers`, `.key`, `.keynote` | L2 DocToText |
| **Rich Text & Emails** | `.rtf`, `.eml` | L2 DocToText |
| **Images & Scanned Documents** | `.png`, `.jpg`, `.jpeg`, `.webp`, `.bmp`, `.tiff`, `.tif`, `.jfif`, `.pnm` | L3 PP-OCRv6 deep learning OCR |
| **Compressed Archives (Roadmap)** | `.zip`, `.tar`, `.gz`, `.7z`, `.rar`, `.bz2`, `.xz` *(UI greyed out)* | Planned: Recursive streaming pipeline |

---

## Building from Source

### Prerequisites
- **Compiler**: GCC 8+, Clang 9+, or MSVC 2019+ supporting C++17
- **CMake**: 3.16 or higher
- **Dependencies**: Qt5 (`Widgets`, `Gui`, `Core`), Cap'n Proto, uchardet, zlib, libxml2, OpenCV

### Linux Quick Start

```bash
# Generate Release configuration
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build in parallel
cmake --build build -j$(nproc)

# Run full test suite
ctest --test-dir build --output-on-failure

# Launch SoEasy
./build/soeasy-gui
```

### Windows Build (MSVC 2019+)

```powershell
cmake -B build -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
.\build\Release\soeasy-gui.exe
```

---

## License

SoEasy is distributed under the [Apache-2.0 License](LICENSE).
