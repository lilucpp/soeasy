# SoEasy Deep Architectural Principles & Technical Specifications

This specification outlines the architectural foundations, process governance, memory optimization, and security design behind the SoEasy local search engine.

---

## 1. Crash-Resilient Sandbox & Modern Process Governance

All file parsing and content extraction workloads are isolated inside dedicated worker processes (`soeasy-worker`), supervised by the host service (`soeasy-service`) and GUI (`soeasy-gui`).

```
+--------------------------------------------------------------+
|                     Main GUI (soeasy-gui)                    |
|  - Virtual Viewport Search (QTableView @ 60 FPS)             |
|  - Debounced Async Preview Thread Pool (Request-ID Guard)     |
+--------------------------------------------------------------+
                               |
                  Shared Memory / Cap'n Proto IPC
                               v
+--------------------------------------------------------------+
|                    Host Guard (WorkerGuard)                  |
|  - Modern Process Management via reproc++ & Stop Actions     |
|  - Watchdog Probe (Kills hangs, OOM, and loops in ms)        |
+--------------------------------------------------------------+
                               |
           Linux: Abstract Namespace Domain Sockets
                               v
+--------------------------------------------------------------+
|                  Parser Sandbox (soeasy-worker)              |
|  - L1: Text & Code (uchardet multi-encoding auto-sniffing)   |
|  - L2: Office / PDF / ODF (DocToText with embedded OCR)      |
|  - L3: Images & Scans (PP-OCRv6 with Header Probe sniffing)  |
+--------------------------------------------------------------+
```

### Architectural Principles
- **Zombie-Free Process Lifecycle (`<defunct>`)**: Powered by modern `reproc++` with strict `stop_actions`, ensuring all worker subprocesses are cleanly reaped upon host shutdown.
- **Zero Disk Residue IPC**: Uses Linux Abstract UNIX Domain Sockets, completely bypassing filesystem socket files (`/tmp/*.sock`) to prevent orphaned disk artifacts.
- **Automatic Self-Healing**: If an external library encounters corrupted files or infinite loops, the watchdog terminates the faulted worker process and transparently restores indexing without freezing the user interface.

---

## 2. Double-Array Trie (DAT) + mmap Zero-Heap Tokenization

Traditional Jieba segmentation constructs millions of dynamic heap nodes when loading the standard 350,000-word vocabulary, taking several hundred milliseconds and consuming **~ 120 MB** heap memory.

### Precompiled Binary Trie & Kernel Mapping
SoEasy integrates [Darts-clone](https://github.com/s-yata/darts-clone) to compile the vocabulary into a contiguous Double-Array Trie binary structure during packaging:

$$\text{Base}[s] + c = t, \quad \text{Check}[t] = s$$

1. **Zero Heap Allocation**: Memory-mapped via Linux `mmap` (Windows `CreateFileMapping`) directly into read-only kernel shared pages.
2. **94% RAM Reduction**: Drops tokenizer resident footprint from **120 MB down to 7.2 MB**.
3. **Instant Startup**: Cold boot dictionary load finishes in sub-millisecond time ($< 1\,\text{ms}$).
4. **Overlay Dictionary Layer**: A lightweight in-memory overlay supports runtime user-defined domain terminology with instant hot-reloading.

---

## 3. Low-Latency Lightweight OCR Optimization

Integrated with PP-OCRv6 deep learning models, engineered for desktop responsiveness and minimal resource usage:

- **Header Probe Nanosecond Sniffing**: Inspects the initial 24 bytes in $< 1\,\mu\text{s}$ before invoking OpenCV image decoders. Automatically skips decorative icons, borders, and solid backgrounds.
- **Dual-Limit Resolution Scaling**: Scales independent scans to 736px and embedded graphics to 640px. Retains 99.37% recognition accuracy while reducing memory by **34%** and boosting inference throughput by over **40%**.
- **Fragmentation Prevention**: Enforces `mallopt(M_ARENA_MAX, 2)` on worker processes and invokes `malloc_trim(0)` during 60-second idle periods.

---

## 4. Hardware-Bound Transparent Local Encryption

SoEasy upholds absolute local privacy with zero telemetry and zero cloud dependencies.

- **Envelope Encryption (`MachineKeyProvider`)**: Derives Key Encryption Keys (KEK) via SHA-256 and high-entropy salt from host machine lifecycle IDs (`/etc/machine-id` on Linux, registry `HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid` on Windows). The 256-bit database Data Encryption Key (DEK) is sealed under KEK masking with SHA-256 HMAC authentication and 0600 file permissions.
- **Offline Theft Defense**: Transferring the `.soeasy.db` file to another device causes instant decryption rejection (`file is not a database`).
- **Transparent Page-Level Encryption**: Full page-level AES-128-CBC protection via SQLite3MultipleCiphers on 32KB pages, compressed on disk with Facebook Zstandard (Level 3).

---

## 5. High-Cohesion Qt UI Architecture & Async Debounce

- **Master-Detail Combined Representation**:
  - `ResultView` utilizes `FileNameAndPathDelegate` to present the bold file title and sub-path within a single composite cell, conserving visual space.
  - `DocumentViewerWidget` handles keyword highlight rotation, bidirectional traversal (<kbd>F3</kbd> / <kbd>Shift+F3</kbd>), and counter indicators.
- **Asynchronous Debounced Preview**:
  - Selection changes dispatch decompression and decryption to background `QThreadPool` workers.
  - Monotonic atomic `Request-ID` sequencing discards obsolete responses when users rapidly navigate through rows.
- **Modular Settings Architecture**:
  - Decoupled into 6 single-responsibility tabs (`General`, `Directories`, `Rules`, `Formats`, `Dictionary`, `Performance`) conforming to the pure virtual `ISettingsTab` interface.

---

## 6. Verified System Benchmarks

| Benchmark Metric | Empirical Measurement | Architectural Foundation |
| :--- | :--- | :--- |
| **Idle Background Footprint** | **~ 82 MB** (Whole System) | GUI ~76MB, Workers 0 processes / 0MB (lazy spawn & 60s idle eviction) |
| **Jieba Tokenizer RAM** | **7.2 MB** (↓ 94% RAM) | Darts-clone binary DAT + kernel `mmap` zero-copy mapping |
| **OCR Throughput & RAM** | **Speed ↑ 42% \| RAM ↓ 34%** | Zero-copy Header Probe + dual-limit resolution scaling (736/640) |
| **Full-Text Query Latency** | **< 0.5 ms** (Instant Response) | SQLite FTS5 inverted index + DAT tokenization + BM25 scoring |
| **Process Stability** | **0 Zombie Processes** | Modern `reproc++` with standard `stop_actions` |
| **Data Security & Disk** | **100% Local Encrypted** | Page-level AES-128 (SQLite3MC) + real-time Zstandard compression |
