# SoEasy Archive Indexing Architecture & Rationale

## 1. Architectural Principle: Keep `doctotext` Pure

SoEasy intentionally **does not** implement archive extraction inside `third_party/doctotext`. `doctotext` remains focused strictly on single-document format parsing (`docx`, `xlsx`, `pptx`, `pdf`, `rtf`, `odt`, `html`, `eml`, plain text, etc.).

### Rationale

1. **Fault & Crash Isolation**: Malformed archives, nested ZIP bombs, or corrupted RAR/7z streams must never bring down the UI process or stall unrelated documents. Putting `libarchive` at the SoEasy orchestration layer allows a dedicated **Unpacker** worker process to isolate container traversal from the N document **Parser** workers (`soeasy-worker`).
2. **Per-Item Timeout Granularity**: A large `.zip`/`.tar.gz` containing 2,000 PDFs cannot be bounded by a single 60-second flat document timeout. Keeping `doctotext` focused on individual file buffers lets SoEasy apply:
   - **No whole-archive size or total wall-clock limit** on the Unpacker container stream (protected instead by an **inactivity/heartbeat watchdog** during active streaming and paused during downstream backpressure).
   - **Strict per-leaf timeout & memory limits** (`max_file_size` + 60s/180s watchdog) on each extracted leaf item inside the parser worker pool.
3. **Zero-Disk-IO Composition**: The Unpacker unpacks nested containers completely in memory (using `archive_read_open_memory` up to depth 6), extracts only user-enabled extensions, and streams chunked leaf buffers (1 MiB chunks) via Cap'n Proto `extractArchiveStream` + `beginBufferParse` directly to parser workers (streaming extracted UTF-8 back in 256 KB chunks).

## 2. Process Isolation & Streaming Flow

```
  IndexOrchestrator (Main / Service Process)
    │
    ├──[1] 1-slot Unpacker (soeasy-worker, kUnpackerWorkerIndex)
    │        └── ArchiveReader (static libarchive 3.8.11)
    │              • Magic-byte format routing + BOM/raw stream sniffing
    │              • Multi-layer nested in-memory walk (max depth = 6)
    │              • Skips unselected formats (archive_read_data_skip)
    │              • Periodic kHeartbeat every 1.5s on slow decompression
    │              • Cap'n Proto stream: onItemMeta / onItemChunk (1 MiB) / onItemEnd -> keepGoing backpressure
    │
    ├──[2] Bounded In-Flight Leaf Queue (capacity = min(2 * worker_count, 6), 64 MiB byte cap)
    │        • Stalls Unpacker via keepGoing/callback backpressure if full
    │
    └──[3] Parser Worker Pool (0 .. worker_count - 1)
             └── WorkerAgent.beginBufferParse(...) -> BufferParseSession (1 MiB chunks) + TextStreamReceiver (256 KB chunks)
                   • L1TextParser / L2OfficeParser (doctotext_process_file_from_buffer_ex) / L3ImageParser
                   • Crash or timeout on leaf #N recycles ONLY that parser worker
                   • Unpacker continues extracting leaf #N+1 .. end unaffected
```

## 3. Storage & Virtual Path Model

SoEasy represents archive members directly as first-class searchable units while keeping database overhead and `NameIndex` RAM usage minimal:

- **Logical Full Path (`files.file_path`)**: Stored directly as `<container_path>/<inner_path>` (e.g., `/data/docs/report.zip/sub/archive.7z/contract.docx`).
  - Preserves existing `GenerateDocId(path)` uniqueness, `NameIndex` directory prefix-trie interning, and prefix-scoped search (`path LIKE '/data/docs/%'`).
  - Eliminates needing a separate full-path string column per child row in SQLite or `NameIndex`.
- **Container Linkage (`files.parent_id` + `archive_containers`)**:
  - Regular files and container rows themselves have `parent_id = 0`.
  - Archive child items store `parent_id = container_id` (`GenerateDocId(container_path)`).
  - Table `archive_containers(container_id, container_path, file_size, mtime, status, item_count)` tracks host container metadata for:
    - Incremental skip (`file_size` + `mtime` match on host archive).
    - Cascade cleanup (`DeleteArchiveContainer`) when host archive is modified, deleted via file watcher, deleted offline (`SweepOfflineDeletedFiles` without calling `std::filesystem::exists` on virtual child paths), or pruned by config changes (`PruneStaleEntriesByConfigLocked`).
    - On-demand top-K `container_path` enrichment in SQLite `DoSearch`, `GetParseIssues`, and `GetContainerPathsByDocIds` (`SearchFilename` / `SearchHybrid`).
- **Container Filename Searchability**: Each host archive also keeps a filename-only record (`parse_level = 0`, `parent_id = 0`) so searching `report.zip` finds the archive itself alongside its indexed members.

## 4. GUI Behavior

- Result list, hover preview popup, preview panel, preview dialog, and parse-issues dialog render archive items using ` › ` breadcrumbs (`FormatArchiveBreadcrumb`) and a compact archive badge.
- Document previews for archive items read strictly from SQLite (`allow_disk_fallback = false`, never probing virtual paths on disk).
- Double-clicking an archive member opens `ContentPreviewDialog` directly; **Open Parent Archive** (`打开所在压缩包`) opens the real host `container_path`, and **Open Folder** reveals the host archive's real directory.
