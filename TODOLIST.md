# SoEasy 需求特性跟踪与优先级排期矩阵 (RTM & Backlog)

> **文档版本**: v1.2.0  
> **设计方法**: Karl Wiegers 分析型需求优先级模型与 Gherkin 验收准则体系  
> **优先级计算公式**: $\text{Score} = \frac{V \times 2 + P}{C + R}$  
> （其中 $V$=相对价值, $P$=缺失惩罚, $C$=实现成本, $R$=技术风险，评分区间 1~9）

---

## 一、 待办特性清单与量化优先级矩阵 (Backlog Matrix)

| 特性代号 | 特性名称 | 相对价值 ($V$) | 缺失惩罚 ($P$) | 实现成本 ($C$) | 技术风险 ($R$) | 综合优先级评分 | 优先级等级 | 当前状态 |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **FEAT-001** | **移除索引目录联动清理底层索引库** | 8 | 8 | 3 | 2 | **4.8** | **P0 (极高)** | 已完成 (已验证) |
| **FEAT-005** | **界面视觉美化与设计语言现代化升级 (对齐 Ant Design 桌面规范)** | 8 | 4 | 4 | 2 | **3.3** | **P1 (高优)** | 待排期 |
| **FEAT-002** | **设置界面提供“自定义分词词典”可视化管理** | 6 | 4 | 4 | 2 | **2.6** | **P1 (中高)** | 已完成 (已验证) |
| **FEAT-006** | **数据库密钥硬件绑定与一机一密零感知自动化** | 8 | 5 | 3 | 2 | **3.6** | **P1 (高优)** | 已完成 (已验证) |
| **FEAT-007** | **引入 SQLiteCpp 现代 C++ 包装库重构底层 SQLite3 裸 C 接口** | 7 | 4 | 5 | 3 | **2.3** | **P1 (中高)** | 已完成 (已验证) |
| **FEAT-003** | **Linux 监控注册阶段物理剪枝与监控库架构升级** | 7 | 8 | 6 | 5 | **2.0** | **P1 (评估中)**| 调研阶段 |
| **FEAT-004** | **压缩包格式（zip/7z/tar 等）正文递归检索** | 5 | 3 | 8 | 8 | **0.8** | **P2 (延后)** | 需求待细化 |

---

## 二、 特性规格、验收准则与追踪关系 (Traceability & Acceptance)

### [FEAT-001] 移除索引目录时联动删除对应的底层索引数据与自适应磁盘空间回收
* **需求层级**：功能需求 (Functional Requirement)
* **业务背景 (实测驱动)**：
  * 当前在配置中已提供移除目录功能，但底层 SQLite3MC 数据库中历史已索引的文件记录未级联剔除；
  * 实测验证 SQLite3MC 在开启批量事务后执行前缀删除效率极高（$<5\text{ms}$），应在目录移除时自动触发前缀清理与内存 `NameIndex` 同步剔除，确保数据一致性与用户隐私物理销毁。
  * 针对超大目录移除后的磁盘释放：采用业界标准方案（`PRAGMA auto_vacuum = INCREMENTAL;` + 后台分批分块 `incremental_vacuum(250)` + `wal_checkpoint(TRUNCATE)`），无需整库独占重写或 2 倍磁盘开销，平滑回收物理空闲页，彻底避免 GUI 冻结。
* **关联模块**：`src/service/index_config.cpp`, `src/service/index_pipeline.cpp`, `src/storage/storage_engine.cpp`
* **关联测试**：`tests/test_storage_engine.cpp`, `tests/test_watch_and_index.cpp`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 移除监控目录后关联索引彻底物理销毁与磁盘物理释放
    Given 用户在设置界面删除了已配置的目录 "/home/user/project_retired"
    When 用户点击确认移除该目录
    Then Service 停止对该目录的后台监听 (RemoveWatch)
    And 存储引擎在单个事务内执行删除所有 "path LIKE '/home/user/project_retired/%'" 的文件记录
    And NameIndex 内存文件名哈希表中清除相应条目
    And 用户在检索框中搜索该目录特有文件时，结果集命中数严格为 0
    And 系统即时截断 WAL 日志并异步分批回收 Freelist 物理页面 (auto_vacuum=INCREMENTAL)
  ```

---

### [FEAT-002] 设置界面提供“自定义分词词典 / 新增分词”可视化管理
* **需求层级**：用户交互与可定制性需求 (Usability)
* **业务背景 (实测驱动)**：
  * `test_dat_jieba` 单元测试实测表明：底层已成功演进为 **Darts-clone DAT 紧凑树 + Overlay 动态内存字典** 双层分词架构；
  * 底层接口已具备微秒级的动态词条插入（`InsertWord`）与即时保存能力，直接在 Qt 设置面板中补充可视化的用户词库编辑表格。
* **关联模块**：`src/storage/dat_jieba.cpp`, `src/gui/settings_dialog.cpp`
* **关联测试**：`tests/test_dat_jieba.cpp`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 用户即时添加行业专有名词免重启生效
    Given 用户打开偏好设置界面的“分词词库”管理选项卡
    When 用户在新增词条框中输入 "极轻量沙箱" 并点击 "添加并生效"
    Then 词条持久化追加至 "user.dict.utf8"
    And 底层 Overlay 字典无须重新加载基础词典即可完成动态热挂载
    And 检索框输入 "极轻量沙箱" 时，FTS5 能够将其作为完整词单元精准召回
  ```

---

### [FEAT-003] Linux 监控注册阶段物理剪枝与监控库架构升级
* **需求层级**：系统级健壮性与物理资源防御 (System Robustness)
* **业务背景 (实测驱动)**：
  * 实测当前机器内核配额 `max_user_watches` 为 119373；
  * 目前 `IndexPipeline` 扫描与 `WatchEngine` 接收端均已实现 $\mathcal{O}(1)$ 拓扑剪枝与后缀前置过滤；但第三方库 `efsw` 在向内核注册 inotify 时，其内部仍会无差别递归遍历子目录，存在打满内核配额的潜在隐患；
  * 评估路径：
    * **方案 A (推荐优先尝试)**：针对 efsw 维护 Inotify 注册剪枝 patch（复用已有的 `IndexConfig` 过滤规则，最小修改面）；
    * **方案 B**：基于 epoll + inotify 轻量定制 Linux 专属监控后端；
    * **方案 C**：调研迁移至 fswatch 过滤后端。
* **关联模块**：`src/service/watch_engine.cpp`, `third_party/efsw/`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 添加含有巨大 node_modules 的工程目录时底层注册剪枝
    Given 待监控的工程根目录下包含 50,000 个小文件的 "node_modules/" 排除目录
    When 调用 AddWatch 添加该工程根目录
    Then 底层监控注册阶段严禁向内核 inotify 为 "node_modules" 及其子目录申请 watch_descriptor
    And 内核已用监控配额增量严格小于工程中非排除目录数
  ```

---

### [FEAT-004] 压缩包格式（zip, tar, gz, 7z, rar 等）正文递归检索
* **需求层级**：深层功能扩展需求 (Extended Capability)
* **业务背景 (实测驱动)**：
  * 实测表明当前系统单进程内存表现极佳（稳定在 54MB），解压大包与海量深层嵌套包极易引发内存暴涨、临时磁盘耗尽（ZipBomb 攻击风险）；
  * 必须先确立严密的防御规格（限制解压最大层级=2、单包提取上限=50MB、解压时限=2s）后方可排期开发。
* **关联模块**：`src/worker/l2_office_parser.cpp`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 安全受限解压并检索 ZIP 压缩包内文档
    Given 用户工作目录中存在一个包含 "contract.docx" 的 "archive.zip" 文件
    When 系统后台流水线扫描到该压缩包
    Then Worker 沙箱安全解包并流式提取文本，临时缓存严格限制在内存阈值内
    And 搜索 "contract" 关键词时能精准召回该压缩包内的文档
  ```

---

### [FEAT-005] 界面视觉美化与设计语言现代化升级 (对齐 Ant Design 桌面规范)
* **需求层级**：用户体验与人机交互需求 (UI/UX)
* **业务背景 (体验驱动)**：
  * 当前原生 Qt5 界面具备良好响应性能，但在视觉层级、边距标尺（Padding/Margin）、色彩对比度与交互状态反馈方面有待建立统一标准；
  * 对齐 Ant Design 4 大核心价值观与桌面客户端规范，统一采用 32px 控件基准高度、14px 主字号、8px 圆角与三层表面拓扑，提供亮色与暗色主题一键无缝热切换能力。
* **关联模块**：`src/gui/main_window.cpp`, `src/gui/search_bar.cpp`, `src/gui/result_view.cpp`, `src/gui/preview_panel.cpp`, `src/gui/settings_dialog.cpp`
* **关联测试**：`tests/test_gui_smoke.cpp`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 应用统一控件标尺与表面层级拓扑
    Given 用户启动桌面客户端并展示主检索窗口
    When 界面完成初始呈现
    Then 搜索栏、按钮与输入框高度统一对齐 32px 物理标尺
    And 搜索结果卡片与右侧预览面板呈现清晰表面层级分界（底色 #FFFAFA，卡片表面纯白，边框细腻微灰）
    And 关键词高亮区域采用高对比度琥珀强调色，字体对比度严格满足 WCAG AA 级标准

  Scenario: 亮色与暗色模式无缝热切换
    Given 用户在设置面板中切换界面主题为“深色模式 (Dark)”
    When 用户点击确认
    Then 所有窗口、表格与预览面板在单帧内完成色彩代号重绘
    And 界面无残存亮色白边或不可读的低对比度文本
  ```

---

### [FEAT-006] 数据库密钥硬件绑定与一机一密零感知自动化
* **需求层级**：安全防护与架构演进需求 (Security & Privacy Automation)
* **业务背景 (安全与体验双轮驱动)**：
  * 原设计要求用户在设置面板手动换密码，但对于 10GB 级数据库，全量重加密需要高额 I/O 耗时与 10GB 临时 WAL 空间，易引发 `SQLITE_FULL` 爆盘且破坏开箱即用体验；
  * 对齐 Chromium / 1Password 工业级最佳实践，引入 **“信封加密 (Envelope Encryption) + 操作系统生命周期锚点绑定”**：
    1. **一机一密**：彻底拔除代码中的硬编码静态口令，每台电脑独立生成专属 256 位高熵随机主密钥（DEK）；
    2. **生命周期绝对不变**：以 Linux 官方标准规范 `/etc/machine-id`（系统安装生成，硬件变更/更新不漂移）与 Windows DPAPI 派生 KEK 密封 DEK，存储为受保护文件（`chmod 0600`）；
    3. **物理防拷贝**：脱机拷贝至其他电脑因 Machine-ID 校验失败而无法解封，数据库物理表现为乱码（`file is not a database`）；
    4. **存量旧库无感平滑迁移**：启动时智能探测，若检测到存量静态口令数据库，后台自动执行 `sqlite3_rekey` 静默升级至本机高熵密钥。
* **关联模块**：`src/common/machine_key_provider.h`, `src/common/machine_key_provider.cpp`, `src/storage/storage_engine.cpp`, `src/gui/main.cpp`
* **关联测试**：`tests/test_machine_key_provider_gtest.cpp`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 首次启动自动生成本机专属高熵加密密钥
    Given 全新安装的 SoEasy 运行环境
    When 系统启动并调用 MachineKeyProvider::GetOrCreateDatabaseKey()
    Then 系统读取本机 /etc/machine-id 密封生成 256 位高熵 DEK
    And 密钥信封落盘为 .db_envelope.key 并设置权限为 chmod 0600
    And 底层数据库以该专属高熵密钥成功创建，物理明文泄露率为 0

  Scenario: 机器脱机窃取防御
    Given 攻击者从目标机器窃取了 soeasy_index.db 与 .db_envelope.key
    When 攻击者在另一台不同 machine-id 的机器上尝试启动解密
    Then 信封校验 MAC 失败，系统拒绝解封 DEK
    And 攻击者无法读取数据库内容，提示 file is not a database

  Scenario: 存量旧版硬编码数据库无缝自愈迁移
    Given 磁盘上存在由旧版静态密钥加密的历史索引数据库
    When 新版客户端启动检测到旧库
    Then 自动使用旧密钥打开并调用 StorageEngine::Rekey() 重加密为本机专属密钥
    And 原旧密钥失效，全量历史文档与索引 100% 完整保留
  ```

---

### [FEAT-007] 引入 SQLiteCpp 现代 C++ 包装库重构底层 SQLite3 裸 C 接口
* **需求层级**：架构演进与代码质量需求 (Architectural Refactoring & Quality)
* **业务背景 (技术与三方库优先原则驱动)**：
  * **现状痛点**：目前 `StorageEngine` 广泛直接调用原生 C API（`sqlite3_prepare_v2`、`sqlite3_bind_*`、`sqlite3_step`、`sqlite3_reset`、`sqlite3_finalize` 等），充斥大量繁琐样板代码、手动错误码检查与类型转换；阶段五虽以 `SqliteStmtScope` RAII 守卫消除了泄漏隐患，但未从根本上实现强类型对象化与现代 C++ 流式封装；
  * **三方库选型**：遵循准则“三方库优先原则”，引入业界成熟、经过广泛实战验证的现代 C++ 包装库 **SQLiteCpp (SRombauts/SQLiteCpp)**，全面提升底层存储的类型安全（Type-Safety）、RAII 确定性析构与代码表现力；
  * **关键技术设计与约束边界**：
    1. **SQLite3MC 物理密态底层无缝对接**：配置 CMake 使 SQLiteCpp 直接复用现有的 `third_party/sqlite3mc` 编译目标与头文件，确保 AES-128-CBC 物理密态存储及 `PRAGMA key` 机制完全透明保留；
    2. **自定义 FTS5 分词器无缝挂载**：现有 `RegisterFts5JiebaTokenizer` 通过获取 SQLiteCpp 的原生 `db.getHandle()` 进行扩展挂载，确保 DAT 结巴分词零改动；
    3. **读写连接池与事务机制重构**：将自研的 `ConnectionPool` 与 `StorageTransaction` 升级为原生托管 `SQLite::Database` 与 `SQLite::Transaction`，利用 C++ 作用域确定性析构彻底保障事务原子性与异常回滚；
    4. **渐进式演进策略**：按“元数据 CRUD -> 批量写入流水线 -> FTS 检索与分页”阶梯式迁移，保持对外 `IStorageEngine` 契约不变，实现无感平滑切换。
* **关联模块**：`CMakeLists.txt`, `third_party/SQLiteCpp/`, `src/storage/storage_engine.h`, `src/storage/storage_engine.cpp`, `src/storage/connection_pool.h`
* **关联测试**：`tests/test_storage_engine.cpp`, `tests/test_storage_transaction_gtest.cpp`, `tests/test_fts5_syntax_gtest.cpp`, `tests/test_connection_pool.cpp`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 使用 SQLiteCpp 强类型与 RAII 语法执行文档批量索引与事务回滚
    Given 存储引擎采用 SQLiteCpp 的 SQLite::Database 与 SQLite::Transaction 封装
    When 业务层开启事务并调用 SQLite::Statement 批量插入文档
    And 在提交前触发系统异常或析构
    Then SQLiteCpp Transaction 析构函数自动执行 ROLLBACK
    And 数据库恢复至原子事务开启前的状态，无死锁与句柄悬挂

  Scenario: SQLiteCpp 依托 SQLite3MC 保持密态物理存储透明可用
    Given 底层链接的目标为带有 AES-128-CBC 加密的 SQLite3MC
    When SQLite::Database 打开加密数据库并执行密钥设置 PRAGMA
    Then 数据库读写操作正常执行，且磁盘物理文件依然为高强度加密形态

  Scenario: 自定义 FTS5 Jieba 分词器与检索两阶段降级在 SQLiteCpp 下无缝工作
    Given SQLiteCpp 数据库句柄成功注册 Jieba FTS5 分词器
    When 通过 SQLite::Statement 执行带有特殊字符（如 "C++"、"lock-free"）的全文检索
    Then 检索顺利执行或安全进入两阶段容错降级，精准召回目标文档
    And 53 项全量单元测试（包含连接池并发、事务隔离、注入防御）全部 100% 通过
  ```

---

## 三、 已闭环历史特性里程碑 (Completed Milestones)

- [x] **[MILE-001] 优化结巴分词内存占用**：引入 Darts-clone 双数组字典树 (DAT) 与 mmap 零拷贝映射，字典物理体积仅 7.24MB，启动耗时降至微秒级，消除旧版 120MB 动态堆内存开销。（由 `tests/test_dat_jieba.cpp` 验证闭环）
- [x] **[MILE-002] 引入 reproc++ 现代进程生命周期治理**：彻底消除 Linux 下 Worker 进程僵死 (<defunct>)，Linux 下切换为内核抽象套接字 IPC 杜绝磁盘残留。（由 `tests/test_sandbox_ipc.cpp` 验证闭环）
- [x] **[MILE-003] 优化 OCR 内存占用与解析性能**：Header Probe 零拷贝尺寸嗅探 (<1μs)、双档位分辨率限边 (普通文档 736 / 内嵌图 640) 与 glibc 内存 Arena 碎片治理。（由 `tests/test_image_ocr.cpp` 验证闭环）
- [x] **[MILE-004] 文件系统边界防御与目录拓扑去重**：Linux `statfs` 虚拟文件系统排除、跨平台卷与节点指纹去重，实现 $\mathcal{O}(1)$ 递归物理剪枝。（由 `tests/test_watch_and_index.cpp` 验证闭环）
- [x] **[MILE-005] 进程文件描述符拉满与 Inotify 监控配额自适应**：免 root 自动提升 `RLIMIT_NOFILE` 至系统硬限制（1048576），智能探测 Inotify 权限并提供配额耗尽自愈指南。（由 `tests/test_system_env.cpp` 验证闭环）
- [x] **[MILE-006] 移除索引目录联动清理底层索引库 (FEAT-001)**：实现 `StorageEngine::DeleteDirectory` 级联物理清理、`NameIndex::RemoveDirectoryPrefix` 内存秒级清除与 `IndexPipeline::RemoveDirectoryIndex` 调度闭环，确保已移除目录文件 0 命中与数据物理销毁。（由 `tests/test_storage_engine.cpp` [Step 9] 与 `tests/test_watch_and_index.cpp` [Step 12] 验证闭环）
- [x] **[MILE-007] 引入 SQLiteCpp 现代 C++ 包装库重构底层 SQLite3 裸 C 接口 (FEAT-007)**：依托 `sqlite3mc` 密态底层实现强类型对象化与 RAII 确定性析构；新增 4 项针对性 GoogleTest 覆盖密钥防御、事务回滚、参数绑定与 FTS5 Jieba 分词器无缝挂载。（由 `tests/test_sqlitecpp_gtest.cpp` 验证闭环）
- [x] **[MILE-008] 设置界面“自定义关键词词库”极简可视化 (FEAT-002) 与大文本高亮 128KB 视窗防御**：
  - **词库界面极简交互**：UI“分词词库”专有 Tab 采用极简单列设计（仅保留关键词列，移除词频微调输入框），隐式自动赋予 10000 默认权重，完美兼容 `user.dict.utf8` 格式；
  - **纳秒级存在性探测拦截 (HasWord)**：`DatJieba` 引入 `HasWord`，通过 Darts-clone DAT 树 `exactMatchSearch` 与 Overlay 字典纳秒级探测系统/用户已存在词条，对重复输入提供友好阻断拦截；
  - **检索自愈能力**：新增词条即时支持分词与检索自愈，点击“重建索引”可触发历史全库重新切词；
  - **大文本视窗截断防御**：`SnippetHighlighter` 引入 128KB 视窗边界截断与零冗余拷贝，消除大文件内存激增。（由 `test_gui_smoke`、`test_dat_jieba` 与 `test_snippet_highlighter_gtest` 验证闭环）
- [x] **[MILE-009] 数据库密钥硬件绑定与一机一密零感知自动化 (FEAT-006)**：
  - **信封加密体系 (DEK + KEK)**：彻底拔除代码中的静态默认口令，每台电脑独立生成专属 256 位高熵随机主密钥（DEK）；
  - **生命周期绝对不变**：以 Linux 官方标准规范 `/etc/machine-id`（系统安装生成，硬件变更/更新不漂移）与 Windows DPAPI 派生 KEK 密封 DEK，存储为受保护文件（`chmod 0600`）；
  - **物理防脱机拷贝**：脱机拷贝至其他电脑因 Machine-ID 校验失败而无法解封，数据库物理表现为乱码（`file is not a database`）；
  - **存量旧库无感平滑迁移**：启动时智能探测，若检测到存量静态口令数据库，后台自动执行 `sqlite3_rekey` 静默升级至本机高熵密钥；
  - **全量测试验收**：新增 6 项针对性 GoogleTest（包含机器码稳定性、首创权限 0600、幂等读取、外机入侵防御、存储引擎集成、旧库迁移），全工程 65 项自动化测试 100% 通过。（由 `tests/test_machine_key_provider_gtest.cpp` 验证闭环）
