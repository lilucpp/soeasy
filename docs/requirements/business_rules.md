# SoEasy 桌面全文检索系统 —— 业务规则目录 (Business Rules Catalog)

> **文档版本**: v1.0.0  
> **编制依据**: Karl Wiegers 业务规则分类体系（事实、约束、动作触发、计算推理）  
> **状态**: Active & Enforced

---

## 一、 业务规则分类体系总览

| 规则代号前缀 | 规则分类领域 | 说明 |
| :--- | :--- | :--- |
| **BR-PRE** | 文件预检与调度规则 (File Prechecking) | 决定文件是否进入深度提取、进入哪一级解析器 |
| **BR-FLT** | 目录边界与物理剪枝规则 (Filtering & Pruning) | 决定递归扫描与监控向哪些文件系统边界止步 |
| **BR-WCH** | 变动监听与事件流转规则 (Filesystem Watching) | 决定实时变动事件的前置拦截与去抖策略 |
| **BR-SYS** | 系统资源与权限降级规则 (System & Limits) | 决定文件句柄、进程配额与降级自愈契约 |
| **BR-STR** | 存储密态与索引生命周期规则 (Storage & Lifecycle) | 决定密态存储、事务批量落盘与清理策略 |

---

## 二、 规则明细清单

### 1. 文件预检与调度规则 (BR-PRE)

#### **[BR-PRE-001] 极小复合二进制文档正文跳过规则**
* **分类**：约束规则 (Constraint)
* **业务逻辑**：当一个文件扩展名属于复合二进制格式（`.doc`, `.ppt`, `.xls`），且文件物理字节大小 $< 1024\text{ 字节 (1KB)}$ 时，系统**严禁将其派发至 L2 深度文本提取沙箱**，直接降级为仅在数据库中记录该文件的基本元数据与文件名索引。
* **设计意图**：合法的复合二进制文档自身头部元数据均超过数百字节。$<1\text{KB}$ 的文件 99% 为损坏断裂文件或临时锁文件，拦截此规则可避免 Worker 解析器进行无意义的解包抛错。
* **对应实现**：`src/service/doc_prechecker.cpp` (`DocPrechecker::Precheck`)
* **验证用例**：`tests/test_office_parser.cpp`

#### **[BR-PRE-002] 超大正文提取文本磁盘溢出 (Spill) 规则**
* **分类**：计算与动作触发规则 (Computation & Action Enabler)
* **业务逻辑**：单次任务共享内存（SHM）槽位默认容量为 $2\text{MB}$。若 L2/L3 解析器提取出的纯文本字节数 $> 2\text{MB}$，解析器自动将正文写入临时 Spill 溢出文件，并仅在共享内存中传递文件描述符；主服务读入完成入库后，必须**同步物理删除该临时溢出文件**。
* **设计意图**：实测证明，如果无限制放大共享内存槽位，会导致 8 个 Worker 占用常驻内存成倍暴增；采用 Spill 机制实现了超大文档可检索与进程内存稳定在 54MB 的完美平衡。实测残留临时文件严格为 0。
* **对应实现**：`src/service/worker_guard.cpp` (`WorkerGuard::OnTaskComplete`)
* **验证用例**：`tests/test_comprehensive_matrix.cpp` [Test 3]

#### **[BR-PRE-003] 图像 OCR 文本有效性过滤规则**
* **分类**：推导规则 (Inference)
* **业务逻辑**：当图像经过 L3 OCR 处理后，若识别提取出的有效中英文字符数 $< 5$ 个，或置信度几何平均得分 $< 0.4$ 时，系统判定该图像为噪点图、壁纸或图标，不将文本写入 FTS5 全文索引。
* **设计意图**：防止无意义的乱码拼音、界面截图图标碎片污染全局倒排索引词典。
* **对应实现**：`src/worker/l2_office_parser.cpp`

---

### 2. 目录边界与物理剪枝规则 (BR-FLT)

#### **[BR-FLT-001] Linux 虚拟文件系统 (VFS) 边界排除规则**
* **分类**：事实与约束规则 (Fact & Constraint)
* **业务逻辑**：在 Linux 环境下，通过 `statfs()` 检测目录所属文件系统魔数。凡属于 `PROC_SUPER_MAGIC` (`/proc`), `SYSFS_MAGIC` (`/sys`), `DEVFS_SUPER_MAGIC` (`/dev`), `CGROUP_SUPER_MAGIC`, `RAMFS_MAGIC` 等伪虚拟文件系统，**系统严禁进入递归遍历与实时监控**。
* **设计意图**：虚拟文件系统包含内核动态生成的文件（如 `/proc/kcore`、无限递归链接），遍历这些目录会导致 CPU 打满、产生虚假事件甚至系统调用挂起。
* **对应实现**：`src/service/index_config.cpp` (`IndexConfig::IsVirtualFilesystem`)
* **验证用例**：`tests/test_watch_and_index.cpp` [Step 9]

#### **[BR-FLT-002] 目录拓扑指纹去重与回环物理剪枝规则**
* **分类**：计算与动作触发规则 (Computation & Action Enabler)
* **业务逻辑**：在遍历扫描目录树时，基于物理设备与节点指纹建立全局访问拓扑表：
  * **Linux**：`uint64_t = (st_dev << 32) | (st_ino & 0xFFFFFFFF)`
  * **Windows**：`uint64_t = (volume_serial << 32) | (file_index_low)`
  若当前目录的指纹已在访问表中存在，直接调用 `iterator.disable_recursion_pending()` 实现真正的 $\mathcal{O}(1)$ 物理剪枝，禁止递归。
* **设计意图**：彻底阻断符号链接互引循环、Junction 卷挂载回环以及重复挂载点导致的死循环遍历。
* **对应实现**：`src/service/index_pipeline.cpp` (`IndexPipeline::ScanDirectoryInternal`)
* **验证用例**：`tests/test_watch_and_index.cpp` [Step 9.2]

---

### 3. 变动监听与事件流转规则 (BR-WCH)

#### **[BR-WCH-001] 非关心后缀前置拦截规则**
* **分类**：动作触发规则 (Action Enabler)
* **业务逻辑**：在跨平台文件变动监控（`WatchEngine::handleFileAction`）收到底层 OS 通知的第一时间，立即提取文件扩展名进行白名单比对。若后缀不属于配置中支持的文本、Office 或图片类型，直接 return 丢弃，**严禁向 EventDebouncer 队列推入该事件**。
* **设计意图**：开发者在工程中频繁触发编译构建时，会瞬间产生数以万计的 `.o`, `.tmp`, `.swp`, `.class` 临时文件。首行过滤拦截可杜绝去抖动队列剧烈膨胀与不必要的 I/O 锁竞争。
* **对应实现**：`src/service/watch_engine.cpp` (`WatchEngine::handleFileAction`)
* **验证用例**：`tests/test_watch_and_index.cpp` [Step 9.1]

#### **[BR-WCH-002] 150ms 时间窗口聚合去抖动规则**
* **分类**：计算与时序规则 (Timing & Computation)
* **业务逻辑**：同一文件路径在 $150\text{ms}$ 窗口内连续触发多次写入/修改事件（如编辑器自动保存、日志追加），系统将其自动聚合归并为单次最终 `MODIFIED` 事件下发。
* **设计意图**：避免大文件在分块写盘阶段触发数十次频繁的全量重解析与数据库重索引。
* **对应实现**：`src/service/event_debouncer.cpp`

---

### 4. 系统资源与权限降级规则 (BR-SYS)

#### **[BR-SYS-001] 进程文件描述符 (FD) 自适应拉满规则**
* **分类**：约束与动作触发规则 (Constraint & Action Enabler)
* **业务逻辑**：
  * **Linux**：进程启动时调用 `getrlimit(RLIMIT_NOFILE)`，并将 `rlim_cur`（软限制）直接提升至 `rlim_max`（硬限制，实测可达 1048576），若为 `RLIM_INFINITY` 则钳制为 1048576；
  * **Windows**：调用 `_setmaxstdio(2048)` 拉至 CRT 最大上限。
  整个过程严禁依赖 root 或管理员权限。
* **设计意图**：保证在万级文件深层递归以及 Worker 多通道 IPC 场景下，文件描述符充足稳定，杜绝 `EMFILE` 错误。
* **对应实现**：`src/common/system_env.cpp` (`SystemEnv::OptimizeProcessFileLimits`)
* **验证用例**：`tests/test_system_env.cpp`

#### **[BR-SYS-002] Linux Inotify 监控配额权限与降级自愈规则**
* **分类**：推导与动作触发规则 (Inference & Action Enabler)
* **业务逻辑**：
  * 检测当前进程是否有修改 `/proc/sys/fs/inotify/max_user_watches` 的写权限（`geteuid() == 0 || access(..., W_OK) == 0`）；
  * **有写权限**：且当前值 $< 524288$ 时，直接覆盖写入提升至 524288；
  * **无写权限（普通用户环境）**：只读获取当前系统配额（实测为 119373）；严禁执行交互式 `sudo`；
  * 当添加监控遇到 `errno == ENOSPC` 耗尽时，系统禁止崩溃退出，向界面与日志输出标准修复命令卡片。
* **对应实现**：`src/common/system_env.cpp`, `src/service/watch_engine.cpp`
* **验证用例**：`tests/test_system_env.cpp`

---

### 5. 存储密态与生命周期规则 (BR-STR)

#### **[BR-STR-001] 页级物理透明加密规则**
* **分类**：事实与约束规则 (Fact & Constraint)
* **业务逻辑**：底层存储引擎必须全局启用 SQLite3MultipleCiphers，采用 AES-128-CBC 算法进行页级物理加密，KDF 迭代次数在首次安全校验通过后设置高效落盘模式。任何裸写或明文旁路存储均属于安全违规。实测裸数据库文件明文匹配率为 0。
* **对应实现**：`src/storage/storage_engine.cpp`
* **验证用例**：`tests/test_storage_engine.cpp` [Step 5]
