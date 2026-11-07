# SoEasy 需求特性跟踪与优先级排期矩阵 (RTM & Backlog)

> **文档版本**: v1.1.0  
> **设计方法**: Karl Wiegers 分析型需求优先级模型与 Gherkin 验收准则体系  
> **优先级计算公式**: $\text{Score} = \frac{V \times 2 + P}{C + R}$  
> （其中 $V$=相对价值, $P$=缺失惩罚, $C$=实现成本, $R$=技术风险，评分区间 1~9）

---

## 一、 待办特性清单与量化优先级矩阵 (Backlog Matrix)

| 特性代号 | 特性名称 | 相对价值 ($V$) | 缺失惩罚 ($P$) | 实现成本 ($C$) | 技术风险 ($R$) | 综合优先级评分 | 优先级等级 | 当前状态 |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **FEAT-001** | **移除索引目录联动清理底层索引库** | 8 | 8 | 3 | 2 | **4.8** | **P0 (极高)** | 待排期 |
| **FEAT-005** | **界面视觉美化与设计语言现代化升级 (对齐 Ant Design 桌面规范)** | 8 | 4 | 4 | 2 | **3.3** | **P1 (高优)** | 待排期 |
| **FEAT-002** | **设置界面提供“自定义分词词典”可视化管理** | 6 | 4 | 4 | 2 | **2.6** | **P1 (中高)** | 待排期 |
| **FEAT-006** | **设置界面配置与修改数据库物理加密密钥** | 7 | 5 | 4 | 4 | **2.4** | **P1 (中高)** | 待排期 |
| **FEAT-003** | **Linux 监控注册阶段物理剪枝与监控库架构升级** | 7 | 8 | 6 | 5 | **2.0** | **P1 (评估中)**| 调研阶段 |
| **FEAT-004** | **压缩包格式（zip/7z/tar 等）正文递归检索** | 5 | 3 | 8 | 8 | **0.8** | **P2 (延后)** | 需求待细化 |

---

## 二、 特性规格、验收准则与追踪关系 (Traceability & Acceptance)

### [FEAT-001] 移除索引目录时联动删除对应的底层索引数据
* **需求层级**：功能需求 (Functional Requirement)
* **业务背景 (实测驱动)**：
  * 当前在配置中已提供移除目录功能，但底层 SQLite3MC 数据库中历史已索引的文件记录未级联剔除；
  * 实测验证 SQLite3MC 在开启批量事务后执行前缀删除效率极高（$<5\text{ms}$），应在目录移除时自动触发前缀清理与内存 `NameIndex` 同步剔除，确保数据一致性与用户隐私物理销毁。
* **关联模块**：`src/service/index_config.cpp`, `src/service/index_pipeline.cpp`, `src/storage/storage_engine.cpp`
* **关联测试**：`tests/test_storage_engine.cpp`, `tests/test_watch_and_index.cpp`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 移除监控目录后关联索引彻底物理销毁
    Given 用户在设置界面删除了已配置的目录 "/home/user/project_retired"
    When 用户点击确认移除该目录
    Then Service 停止对该目录的后台监听 (RemoveWatch)
    And 存储引擎在单个事务内执行删除所有 "path LIKE '/home/user/project_retired/%'" 的文件记录
    And NameIndex 内存文件名哈希表中清除相应条目
    And 用户在检索框中搜索该目录特有文件时，结果集命中数严格为 0
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

### [FEAT-006] 设置界面配置与修改数据库物理加密密钥
* **需求层级**：安全防护与用户可控性需求 (Security & Privacy)
* **业务背景 (安全驱动)**：
  * 当前底层存储全局启用 SQLite3MC (AES-128-CBC) 加密，但默认采用内部密钥派生；
  * 为践行“数据零上云、用户独占所有权”原则，系统在设置面板中开放用户自定义加密主口令配置，并提供安全的旧密钥校验与原子重加密（`sqlite3_rekey`）功能，杜绝物理拷贝数据库引发的明文泄露。
* **关联模块**：`src/storage/storage_engine.cpp`, `src/gui/settings_dialog.cpp`
* **关联测试**：`tests/test_storage_engine.cpp`
* **验收准则 (Acceptance Criteria - Gherkin)**：
  ```gherkin
  Scenario: 用户在设置面板中更新数据库加密密钥
    Given 用户打开设置面板的“安全与密态存储”选项卡
    When 用户输入正确的旧主口令并输入符合强度要求的新口令（长度不低于 8 位）
    And 用户点击“更新密钥并重新加密”
    Then 存储引擎在单个事务内调用 SQLite3MC 原子重加密接口完成全库重混淆
    And 重加密完成前若发生系统异常，底层数据库文件回滚至原密钥状态，无损毁损坏
    And 弹出成功提示并通知用户后续访问将依赖该新口令

  Scenario: 输入错误旧密钥时拒绝重加密
    Given 用户尝试在设置面板修改加密密钥
    When 用户输入的旧密钥与数据库实际密钥校验不一致
    Then 系统立即终止重加密流程并弹出“原密码校验失败”告警提示
    And 底层数据库保持原加密状态不变
  ```

---

## 三、 已闭环历史特性里程碑 (Completed Milestones)

- [x] **[MILE-001] 优化结巴分词内存占用**：引入 Darts-clone 双数组字典树 (DAT) 与 mmap 零拷贝映射，字典物理体积仅 7.24MB，启动耗时降至微秒级，消除旧版 120MB 动态堆内存开销。（由 `tests/test_dat_jieba.cpp` 验证闭环）
- [x] **[MILE-002] 引入 reproc++ 现代进程生命周期治理**：彻底消除 Linux 下 Worker 进程僵死 (<defunct>)，Linux 下切换为内核抽象套接字 IPC 杜绝磁盘残留。（由 `tests/test_sandbox_ipc.cpp` 验证闭环）
- [x] **[MILE-003] 优化 OCR 内存占用与解析性能**：Header Probe 零拷贝尺寸嗅探 (<1μs)、双档位分辨率限边 (普通文档 736 / 内嵌图 640) 与 glibc 内存 Arena 碎片治理。（由 `tests/test_image_ocr.cpp` 验证闭环）
- [x] **[MILE-004] 文件系统边界防御与目录拓扑去重**：Linux `statfs` 虚拟文件系统排除、跨平台卷与节点指纹去重，实现 $\mathcal{O}(1)$ 递归物理剪枝。（由 `tests/test_watch_and_index.cpp` 验证闭环）
- [x] **[MILE-005] 进程文件描述符拉满与 Inotify 监控配额自适应**：免 root 自动提升 `RLIMIT_NOFILE` 至系统硬限制（1048576），智能探测 Inotify 权限并提供配额耗尽自愈指南。（由 `tests/test_system_env.cpp` 验证闭环）
