# 从 954 MiB 峰值到 35 MiB 匿名私有内存：SoEasy 桌面端内存治理实战与 Linux 内存观测指南

> 本文复盘 `SoEasy` 文件搜索与预览工具在 Linux 环境下的一次完整内存治理实践：从用户反馈“**明明之前调优过，但 `btop` 里常驻内存还是飙到 694M、重建索引峰值近 1 GiB**”出发，系统介绍 Linux 内存核心指标口径、现场 `/proc` 诊断脚本、打破直觉的压测发现，以及历经三轮架构收敛的工程实践。

---

## 引言：一个反直觉的现场警报

项目早已做过一轮基础 SQLite 参数调优，但实机观察 `dist/soeasy-gui`（索引约 `2.46 万` 个文件、加密索引库体积约 `306 MB`）时，监控工具里的数字依然触目惊心：

- **`btop` / `top` 常驻内存（RES）**：闲置状态下高达 `694 MiB`（`712,712 kB`）
- **历史最高水位（VmHWM）**：全量重建索引期间冲到 `954 MiB`（`977,280 kB`）

面对这种问题，最忌讳“拍脑袋猜哪里泄露了”。很多时候，**你看到的“内存很高”，是由统计口径偏差、glibc 分配器缓存策略、文件系统页缓存误判以及冷热数据耦合共同放大出来的组合效应**。

要精准“拆弹”，首先得把 Linux 内存的度量衡对齐。

---

## 一、内功心法：Linux 内存基础概念与观测工具链

在动手查问题前，先结合常见系统性能观测工具（`top`、`vmstat`、`iostat`、`pidstat`）和 `/proc/<PID>/` 体系，厘清几个最容易混淆的内存概念。

### 1.1 五大高频内存指标：VIRT、RES、SHR、PSS、USS

| 指标 | `/proc` 对应字段 | 含义与物理意义 | 排查时的关注重点 |
| :--- | :--- | :--- | :--- |
| **VIRT** | `VmSize` / `VmPeak` | **虚拟地址空间总大小**（含未分配物理页的 `mmap` 保留区、`PROT_NONE` 保护页、线程栈映射等）。 | 一般不直接反映真实物理内存消耗，但 `VmPeak` 过大会暴露地址空间碎片或大块无节制映射。 |
| **RES** | `VmRSS` | **常驻内存集（Resident Set Size）**，进程当前**真实占用物理内存（RAM）**的总量（含与其他进程共享的 `.so` 与 `/dev/shm`）。 | `top`、`htop`、`btop` 默认最显眼的数字，但多进程共享同一库时会被重复计算。 |
| **SHR** | — | **共享内存大小**，当前进程 `RES` 中可能与其他进程共享的部分（动态库代码段、字体映射、`tmpfs` / `/dev/shm`、文件 `mmap`）。 | 单个进程的真实独占内存通常近似于 `RES - SHR`。 |
| **PSS** | `Pss`（`smaps_rollup`） | **按比例分摊常驻内存（Proportional Set Size）**：私有页全额计入，共享页按当前共享进程数平分（例如被 4 个进程共享的 `40 MiB` `.so`，每个进程算 `10 MiB`）。 | **评估“整机多开/系统总物理内存真实开销”的黄金指标**，所有进程 PSS 之和约等于全系统真实耗用的物理页。 |
| **USS** | `Private_Clean + Private_Dirty` | **进程唯一独占物理内存（Unique Set Size）**，即该进程独享的物理页。 | **回答“如果现在 `kill -9` 这个进程，操作系统能立刻收回多少物理内存”**。 |

### 1.2 解剖 VmRSS 的三大组成与高水位线 `VmHWM`

在 `/proc/<PID>/status` 中，Linux 内核把最核心的常驻内存 `VmRSS` 精确拆成了三部分：

$$VmRSS \approx RssAnon + RssFile + RssShmem$$

1. **`RssAnon`（匿名私有内存）**
   - 没有后端文件对应的纯内存页，主要是 `[heap]`（glibc `malloc` 堆）、线程栈以及 `[anon:rw-p]`（大块 `malloc` 或内部 `mmap(MAP_ANONYMOUS)` 分配的数据结构、临时排序缓存、STL 容器缓冲）。
   - **判断准则**：它**不能被内核通过丢弃页缓存直接回收**（除非进入 Swap），且几乎近似于进程独占的私有内存：
     $$RES - SHR \approx RssAnon$$
   - 降内存的首要靶心永远是 `RssAnon`。
2. **`RssFile`（文件映射内存）**
   - 有磁盘后端文件支撑的常驻物理页：包括可执行程序本身、各类系统动态库（如 GUI 渲染依赖的 `libLLVM.so.20.1`、Qt 库）、中文字体文件（如 `NotoSansCJK`）以及业务主动发起的文件 `mmap`（如 SQLite `db_mmap`）。
   - **判断准则**：干净的文件页（`Private_Clean` / `Shared_Clean`）在系统内存吃紧时内核可直接回收；但如果是单个大独占数据库做了几百 MB 的 `mmap`，虽然“可回收”，仍会在监控面板中把进程 `RES` 和 `PSS` 顶得很高。
3. **`RssShmem`（共享内存与 `tmpfs`）**
   - 进程映射的 System V / POSIX 共享内存（`/dev/shm/*`）以及 `memfd`、`tmpfs` 页。
   - **注意误区**：`RssShmem` 在 `top` 中会被算进 `SHR`，但如果这部分共享内存只有你自己或主从两三个 Worker 在用，且写满脏页后不回收，它就会实打实长期焊死物理 RAM（直到进程销毁或主动归还物理页）。
4. **`VmHWM`（High Water Mark，常被口误拼为 `wmhwm`）**
   - 全称 **Virtual Memory High Water Mark**，是 `/proc/<PID>/status` 中记录的**进程自启动以来 `VmRSS` 达到过的历史最高物理峰值**（只增不减）。
   - **为什么死盯 `VmHWM`？** 很多后台程序平时 `VmRSS` 看起来只有 `150 MiB`，但批处理/重建索引瞬间 `VACUUM` 或大数组扩容冲出了 `950 MiB` 的 `VmHWM`；在低配机器或容器环境下，触发 OOM Killer 杀死进程的正是这一瞬间的 `VmHWM`！

### 1.3 常见口径疑云与多维度排查命令

> **为什么 `top` 里看到的 `RES` 比桌面系统监视器里高出一大截？**
>
> 因为系统监视器（如 GNOME System Monitor 等）默认展示的是**近似私有内存**（更接近 `RES - SHR` 或 `RssAnon` + 私有脏页），自动扣除了几十上百兆的共享 `.so` 和公共字体映射；而 `top` / `btop` 展示的是全量 `RES`，把 `libLLVM`、`NotoSansCJK`、数据库文件 `mmap` 全算在了单个进程头上。

在把问题收窄到具体进程之前，通常先用系统级命令行排除外部干扰（例如是不是系统内存整体耗尽导致换页抖动，或降内存方案意外换来了恐怖的磁盘 I/O 写放大）：

- **`vmstat 1`**（系统全局内存与 Swap 脉搏）：关注 `free`、`buff`、`cache`，核心盯着 **`si` / `so`**（Swap In / Swap Out，只要持续非零，说明物理内存已经兜不住开始疯狂换页）以及 **`bi` / `bo`**（块设备每秒读写块数）。
- **`iostat -d -x 1`**（磁盘写放大与阻塞验证）：优化内存时最忌讳“按下葫芦浮起瓢”，当缩小写入批大小时，务必盯紧 `r/s`、`w/s`、`rkB/s`、`wkB/s`、`r_await`、`w_await` 和 `%util`。
- **`pidstat -r -d 1 -p <PID>` & `lsof -p <PID>`**：按秒级跟踪目标进程缺页中断（`minflt/s`、`majflt/s`）与真实磁盘写入量，配合 `lsof` 查看进程打开的大体积 `/dev/shm`、WAL 或临时文件句柄。
- **`top` 实战常用快捷键**：
  - `M`：按 `RES` 物理内存占用降序排列；`P`：按 CPU 降序
  - `E` / `e`：切换顶部汇总区与进程列表区的内存单位（KB/MB/GB）
  - `H`：展开线程视图（排查多线程 glibc Arena 膨胀时配合观察线程数）
  - `c`：显示完整启动命令行；`W`：保存当前视图配置

---

## 二、现场诊断：我们是如何抓取“三大元凶”的？

回到 `SoEasy` 的现场。针对正在运行的异常进程（`PID 77286`），我们没有急着翻代码，而是直接向 Linux `/proc` 文件系统要第一手物理证据。

### 2.1 一键聚合脚本：让 `/proc/status` 与 `smaps` 开口说话

以下这段 shell + `awk` 脚本是本次排查的“CT 扫描仪”，能在不重启进程的情况下把常驻内存按来源文件/匿名段精确归类：

```bash
PID=$(pgrep -xo "soeasy-gui" || pgrep -f "soeasy-gui" | head -n1)

# 1. 查看内存全局摘要与三驾马车拆解 (VmHWM / VmRSS / RssAnon / RssFile / RssShmem)
grep -E "^(Name|Threads|VmPeak|VmSize|VmHWM|VmRSS|RssAnon|RssFile|RssShmem|VmData)" /proc/$PID/status

# 2. 查看整进程的 PSS 与私有脏页总量
cat /proc/$PID/smaps_rollup

# 3. 按映射对象聚合 smaps，定位 Top 15 内存大户
awk '
/^[0-9a-f]+-[0-9a-f]+ / {
  if (rss > 0) {
    key = (path == "" ? "[anon:" perm "]" : path)
    tot_rss[key] += rss; tot_pss[key] += pss; tot_anon[key] += anon; cnt[key]++
  }
  perm = $2; path = (NF>=6 ? $6 : ""); rss = pss = anon = 0
}
/^Rss:/ { rss = $2 }
/^Pss:/ { pss = $2 }
/^Anonymous:/ { anon = $2 }
END {
  if (rss > 0) {
    key = (path == "" ? "[anon:" perm "]" : path)
    tot_rss[key] += rss; tot_pss[key] += pss; tot_anon[key] += anon; cnt[key]++
  }
  for (k in tot_rss) printf "%9d KB RSS | %9d KB PSS | %9d KB Anon | %3d maps | %s\n", tot_rss[k], tot_pss[k], tot_anon[k], cnt[k], k
}' /proc/$PID/smaps | sort -rn | head -n 15
```

### 2.2 静态解剖实录（`PID 77286`）

在 `btop` 报 `694M` 的现场，上述脚本立刻给出了毫无水分的拆解账本：

- **`/proc/77286/status` 与 `smaps_rollup`**：
  - `Threads`: `18`
  - `VmPeak`: `2,564,436 kB` | `VmSize`: `2,187,240 kB`
  - **`VmHWM`**: **`977,280 kB`**（约 **`954 MiB`**）
  - **`VmRSS`**: **`712,712 kB`**（约 **`696 MiB`**）
  - **`Pss`**: **`620,104 kB`**（约 **`605.6 MiB`**，`Private_Dirty: 322,304 kB`，`Private_Clean: 286,980 kB`）
  - 组成明细：`RssAnon = 316,156 kB`（`308.7 MiB`）、`RssFile = 386,656 kB`、`RssShmem = 9,900 kB`
- **`smaps` 聚合 Top 热点直接锁定三大元凶**：
  1. **加密库独占 `db_mmap`**：`soeasy_index.db` 单文件就吃掉了 **`256,576 kB` RSS / `256,576 kB` PSS**（几乎撑爆 `RssFile`）。
  2. **69 块神秘匿名大块 `[anon:rw-p]`**：合计 **`222,336 kB`**，拆开看竟然藏着 `65,536 kB`（`64 MiB`！典型 glibc Thread Arena）、`63,244 kB`、`48,920 kB`、`20,112 kB`、`15,772 kB` 等巨型碎块。
  3. **`[heap]` 传统堆**：占 **`83,308 kB`**。

### 2.3 动态追踪重建过程（`PID 84074` 每 2 秒采样时间线）

静态快照只能看到“案发现场的结果”，为了看清 `VmHWM = 954 MiB` 的尖刺到底是在哪一步代码刺出去的，我们在触发全量重建（`24,586` 个文件）期间，用脚本以 **2 秒间隔** 连续记录 `/proc/<PID>/status`、`smaps`（`[heap]`、`[anon:rw-p]` 总量与最大单块、`db_mmap`）以及磁盘 `-wal` 文件体积，同时用 `/proc/<PID>/io` 监测真实写盘量（`write_bytes`）：

| 时间点 | 阶段事件 | `VmRSS` | `RssAnon` | `[heap]` | `[anon:rw-p]` (最大单块) | `db_mmap` | `-wal` 大小 | 关键发现 |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| `12:35:18` | 冷启动闲置 | `174.8 MiB` | `56.4 MiB` | `32.6 MiB` | `13.5 MiB` | `0` | `0 B` | 此时还没把库读热。 |
| `12:35:25` | `DoClearAll + VACUUM` | `423.3 MiB` | **`301.1 MiB`** (`+240.8`) | — | `254.3 MiB` (**`118.6 MiB`**) | — | `305.3 MiB` | **铁证 1**：SQLite 默认 `temp_store = MEMORY`，对 300MB 库做 `VACUUM` 瞬间在内存构造百兆临时表！ |
| `12:35:41` ~ `12:36:19` | 扫描并写库 | `508 -> 781.5 MiB` | `334 -> 398.3 MiB` | — | `287 -> 351.4 MiB` | `0 -> 250.4 MiB` | `320~324 MiB` | 多线程并发扫描期间大量创建 `64 MiB` glibc Arena 且 `db_mmap` 逐步填满。 |
| `12:36:21` | Flush + WAL Checkpoint | `858.9 MiB` | `475.7 MiB` | — | `363.4 MiB` | `250.4 MiB` | — | **历史最高水位冲顶：`VmHWM = 949.0 MiB`**！ |
| `12:36:23` ~ `12:36:50` | 闲置稳态 | `701.1 MiB` | `317.9 MiB` | `72.3 MiB` | `233.3 MiB` | `250.4 MiB` | — | **铁证 2**：任务早结束了，但 glibc Arena 和高地址页死活不还给内核！ |

---

## 三、方案权衡与三轮演进（Why > What）

查清物理证据后，我们没有做“一刀切砍功能”的妥协，而是结合基准测试与系统数据结构设计，分三轮对不同层级进行了外科手术式治理。

### 3.1 第一轮（底座止血：`8e12d5a`）：Allocator 约束、SQLite 策略与“反直觉”的压测结论

#### ① glibc `ptmalloc2` 多线程 Arena 驯服
为什么重建结束十几分钟，程序不干活了，`[anon:rw-p]` 依然留着 `233 MiB` 甚至 `65,536 kB` 整块？
这是 Linux glibc `malloc`（`ptmalloc2`）的经典机制：为降低多线程锁竞争，默认每个进程最多可扩展到 `8 * CPU 核数` 个 `Arena`（64 位下默认按 64 MiB 对齐映射）。只要某个线程在写库高峰期在一个 Arena 高地址留下一个 tiny chunk 没释放，整个几十 MB 的 Arena 就无法通过 `brk` 或 `munmap` 退还给内核。

- **权衡**：上 `jemalloc`/`mimalloc` 会引入额外构建和依赖耦合；对于并发写线程本就有限的桌面应用，控制 Arena 数量即可收奇效。
- **方案**：在 [main.cpp](../src/gui/main.cpp) 进程最早入口处（任何线程创建前）调用 `mallopt(M_ARENA_MAX, 2)`（由 `#if defined(__linux__) && defined(__GLIBC__)` 守卫），并在闲置收敛时（`ShrinkIdleMemoryUnlocked`）显式触发 `malloc_trim(0)` 强制将自由碎片退回 OS。
- **实证对比**：Stage A 实验验证 `M_ARENA_MAX = 2` 时 `RssAnon = 37.73 MiB`，显著低于 `M_ARENA_MAX = 4` 的 `53.21 MiB`。

#### ② `perf/memory-profile-bench` 推翻两个直觉误区

在动手改写 SQLite 参数前，我们在 Release (`-O3`) 下跑了 Stage A（2,995 文件，26 组配置）和 Stage B（`24,639` 文件、`305.4 MiB` 加密库）压测矩阵，**两条直觉被数据直接推翻**：

1. **误区 1：“开启 `mmap` 能零拷贝加速搜索，多占点 `RssFile` 也值”**
   - 我们在加密库上对比了 `B0_baseline`（`mmap=256M`）、`B4`（`mmap=64M`）和 `B2`（`mmap=0`）：

| 配置组 | 索引耗时 | `VmHWM` | 闲置 `VmRSS` (`Anon` / `mmap`) | 写盘量 `write_bytes` | FTS 搜索均值 / P95 |
| :--- | :---: | :---: | :---: | :---: | :---: |
| `B0_baseline` (`500`/`100`/不限队列, `4` 读者, `mmap=256M`, 默认 Arena) | `57.42 s` | `619.86 MiB` | `479.45 MiB` (`207.07` / `250.59`) | `4965.29 MiB` | `5.68 / 20.35 ms` |
| `B4` (`mmap=64M`) | `66.43 s` | `381.74 MiB` | `116.73 MiB` | — | — |
| **`B2` (`kLowMemory` 默认: `150`/`100`/`16M`, `2` 读者, `mmap=0`, `M_ARENA_MAX=2`)** | **`60.83 s`** | `365.23 MiB` | **`53.54 MiB`** (`31.59` / `0.09`) | `5074.89 MiB` | **`4.26 / 16.21 ms`** |
| `B11` (`kLowWrite`: `300`/`200`/`32M`, `wal=10000`, `mmap=0`) | `68.69 s` | **`344.66 MiB`** | `57.76 MiB` (`35.72`) | **`3998.30 MiB`** (`-21%`) | `4.61 / 17.95 ms` |

   - **真相**：项目底层用的是带 **AES-128 加密的 SQLite3MC**！加密页**根本无法让用户态直接读取零拷贝**，每次读取依旧要拷贝并解密进 SQLite Page Cache。开启 `mmap=256M` 不仅白白让 `250.59 MiB` 密文文件页赖在进程 `VmRSS` 里，搜索均值（`5.68 ms`）甚至还不如 `mmap=0`（`4.26 ms`）。于是果断把所有 Profile 的 `mmap_bytes` 全部归零！

2. **误区 2：“为了省队列内存，把 `batch_size` 调得越小越好”**
   - Stage A 实测发现：把异步写入队列的 `batch_size` 压到 `25` 或 `50` 时，虽然队列内存降了，但频繁事务提交让磁盘 **`write_bytes` 分别暴涨至 `366.05 MiB` / `272 MiB`**（`batch_size = 100~200` 时仅 `171~211 MiB`）。
   - **修正方案**：保留 `queue_batch_size >= 100` 保住磁盘写放大底线，改由 **`queue_max_bytes`（16 MiB / 32 MiB）做精确字节上限背压**，在生产端控死尖峰内存；同时在 [resource_profile.h](../src/common/resource_profile.h) 中沉淀为 `kLowMemory`（默认极低内存）与 `kLowWrite`（降写放大 `-21%`）双档策略，并配合 `DoClearAll` 执行 `VACUUM` 时临时切为 `PRAGMA temp_store = FILE` + 完成后 `PRAGMA wal_checkpoint(TRUNCATE)` 与 `PRAGMA shrink_memory` / `sqlite3_db_release_memory`，彻底干掉 `118.6 MiB` 临时表尖刺。

---

### 3.2 第二轮（暗漏清理：`7f1f0e2`）：静止共享字典、惰性 `tmpfs` 回收与早释放

第一轮之后，全程序 `dist/soeasy-gui` 在 `2.46 万` 文件下的闲置 `VmRSS` 已从 `701 MiB` 骤降到 `246.15 MiB`（`PSS 151.41 MiB`，`RssAnon 103.49 MiB`）。但细查 `smaps` 仍有 `6.00 MiB` 的 `/dev/shm/soeasy_shm_*` 与多份重复分词词典驻留。

1. **分词词典从“多实例各自加载”到 `DatJieba::Freeze()` 只读单例**
   - 原先不同写入/查询 FTS5 分词器上下文各自持有 Jieba 字典内存，且存在冗余静态字典开销；在 [dat_jieba.h](../src/storage/dat_jieba.h) / [fts5_jieba_tokenizer.cpp](../src/storage/fts5_jieba_tokenizer.cpp) 中通过 `DatJieba::Freeze()` 构建只读共享单例 `AcquireSharedJieba()`，多线程零拷贝复用同一份词典。
2. **POSIX 共享内存 `ShmSlotPool`：OS 页对齐 + `madvise(MADV_REMOVE)`（`6.00 MiB -> 12 kB`）**
   - 原本 Master 与 Worker 之间预建了 `3 * 2 MiB = 6 MiB` 共享内存槽位，即便 Worker 空闲退出，脏页依然赖在 `tmpfs`（计入 `RssShmem`）里不释放。
   - **解法**（见 [shm_pool.h](../src/common/shm_pool.h) 与 [shm_pool.cpp](../src/common/shm_pool.cpp)）：
     - 将每个槽位跨度按操作系统页大小对齐，利用 Linux 按需缺页机制实现写多少才占多少物理页；
     - 在 Worker 守卫线程检测到全部槽位空闲进入休眠时，调用 `ReclaimIdlePages()` 对空闲 Payload 区域执行 **`madvise(..., MADV_REMOVE)`**。在 Linux `tmpfs` / POSIX SHM 上，`MADV_REMOVE` 会直接从页缓存中挖洞释放背后的物理内存页（而不解除虚拟映射）。
   - **效果**：冷启动 `SHM` 占用从 `6.00 MiB` 断崖降至 **`12 kB`**；重建期间峰值 `2.44 MiB`（`2,496 kB`），60 秒空闲回收后再次回落到 **`12 kB`（`-99.8%`）**！
3. **扫盘列表显式收缩容量**：在 [index_orchestrator.cpp](../src/service/index_orchestrator.cpp) 的扫描收尾阶段使用 `std::vector<std::string>().swap(files_to_index)` 彻底归还大容量 `std::vector` 缓冲区，而非仅仅 `.clear()`。

---

### 3.3 第三轮（数据结构重塑：`ad40e70`）：Everything 启发与 `NameIndex` SoA 紧凑化

前两轮完成后，进程内依然存在一份体积可观的内存文件名索引 `NameIndex`（每文件用传统 OOP 对象 + `std::string` 存储全路径、文件名及各类散列/辅助索引，每文件高达 **`475.78 B/file`**，`10 万` 文件即需 `45.36 MiB` 碎片化堆内存）。

参考 **Everything** 的极简内存模型以及系统级数据结构设计原则（`systems-data-design`）：**连续紧凑优于离散指针、热路径只存索引不存长字符串、冷热物理隔离**，我们重塑了 [name_index.h](../src/service/name_index.h) / [name_index.cpp](../src/service/name_index.cpp)：

#### ① 为什么放弃直接 `mmap` 元数据，而选择纯内存冷热分离 SoA + Arena？
在加密/跨平台场景下，如果将索引指针关系直接映射在文件上，极易在高频前缀/子串遍历时引发密集磁盘随机缺页或结构版本耦合；相比之下，将数据本身在内存里压缩到极致的顺序连续数组（**Structure of Arrays, SoA**），既能让 L1/L2 Cache 命中率最大化，又能让每百万文件开销可控在百兆出头。

#### ② 零填充 SoA 与连续字符池布局
- **热路径（搜索扫描高频访问）**：`FileHotEntry` 严格控制在 **12 字节**（包含父目录 ID 与紧凑池化偏移量等高频搜索所需最小集），大量顺序扫名查询全程只扫这块极小连续数组，完全不触碰冷字段；
- **冷路径（按命中结果 ID 回查属性）**：`FileColdMeta` 拆为独立的 **24 字节** 数组，仅在真正命中的几十/几百条结果渲染时才按索引取用；
- **目录树压缩**：`DirNode` 仅占 **12 字节**，路径按父目录链表复用存储，不再对每个文件重复存整串父目录字符串；
- **平坦哈希表**：`DocIdHashMap` 每桶压缩至 **12 字节**；全量文件名堆入统一的连续字节 Arena，彻底消灭几十万个 `std::string` 带来的 `malloc` 头部元数据（每个堆块 16B+ 开销）和内存碎片。

#### ③ 实测微基准对比与压测“意外抓虫”
在 `Release -O3`（`M_ARENA_MAX=2`）微基准 [test_name_index_gtest.cpp](../tests/test_name_index_gtest.cpp) 中，成效立竿见影：

| 测试规模 | 改造前 RSS 增量 | 改造后 RSS 增量 | 单文件真实 RSS 开销 | 内部有效负载净重 | 构建耗时 | 平均搜索耗时 |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **24,639 文件** | `11.18 MiB` (`475.78 B/file`) | **`3.59 MiB`** | **`152.94 B/file`** | `2.66 MiB` (`113.40 B/file`) | `22.21 -> 15.45 ms` | **`0.32 ms`** |
| **100,000 文件** | `45.36 MiB` | **`14.58 MiB`** | **`152.86 B/file`** | `10.66 MiB` (`111.73 B/file`) | `103.82 -> 66.98 ms` | **`1.27 ms`** |

> **一次极具价值的压测反转：`std::shared_mutex` 读者饿死写者**
>
> 在编写并发单元测试 `ConcurrentReadersAndWriter` 时，高频多读者连续读取居然把后台写者活生生堵死。根因在于 Linux glibc 下默认 `std::shared_mutex`（底层 `pthread_rwlock` 默认属性）采用**读者优先（Reader-Preferring）**，只要读流不断，新到的读锁可以直接插队到等待写锁前面。
>
> **解法**：封装原生 `pthread_rwlock_t` 实现 `SharedRwLock`，显式配置属性 **`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`**（写者优先且禁止同线程递归加读锁）。修改后，写者不再饥饿，并发重建与查询同时跑稳如磐石。

---

## 四、优化效果总账与可复用工具箱

### 4.1 同一数据集（~2.46 万文件）端到端实机演进总账

下面这张表记录了真正编译出的桌面端 `dist/soeasy-gui` 在相同数据集上的逐轮 `/proc/<PID>` 实机数字（包含 Qt 桌面框架、C++ 运行时、`libLLVM` 图形栈及字体等全部真实开销）：

| 版本阶段 | 场景状态 | `VmRSS` | `PSS` | `RssAnon` (硬核私有内存) | `RssFile` | `RssShmem` (含 `/dev/shm`) | 峰值 `VmHWM` |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **初始问题态** (`PID 77286` / `84074`) | 重建后稳态 | `696 ~ 701 MiB` | `605.6 MiB` | **`308.7 ~ 317.9 MiB`** | `386.6 MiB` | `9.9 MiB` | **`949 ~ 954 MiB`** |
| **Round 1 (`8e12d5a`)**<br>底座 + `M_ARENA_MAX=2` + `mmap=0` | 冷启动闲置<br>重建后+搜索稳态 | `181.82 MiB`<br>`246.15 MiB` | `95.52 MiB`<br>`151.41 MiB` | `57.50 MiB`<br>`103.49 MiB` | `112.45 MiB`<br>`132.28 MiB` | `11.87 MiB`<br>`10.38 MiB` (`SHM 6.0M`) | `468.48 MiB` |
| **Round 2 (`7f1f0e2`)**<br>Jieba 单例 + `MADV_REMOVE` | 冷启动闲置<br>重建后+60s 回收 | `161.83 MiB`<br>`191.48 MiB` | `79.21 MiB`<br>`105.93 MiB` | `48.97 MiB`<br>`66.21 MiB` | — | `SHM 12 kB`<br>`3.68 MiB` (`SHM 12 kB`) | `465.43 MiB` |
| **Round 3 (`ad40e70`)**<br>Everything SoA `NameIndex` | 冷启动闲置<br>（含完整界面栈） | **`148.11 MiB`**<br>（`151,668 kB`） | **`66.15 MiB`**<br>（`67,741 kB`） | **`35.30 MiB`**<br>（`36,144 kB`） | 含 `libLLVM.so` 等共享栈 | `SHM 12 kB` | **`151.28 MiB`**（冷态） |

**核心收益一览**：
- 实机冷启动的 **`RssAnon`（真正不可轻易回收的私有内存）从最初的 `308.7 ~ 317.9 MiB` 一路打到 `35.30 MiB`，削减近 `89%`**！
- 重建全过程最高水位 `VmHWM` 从 **`954 MiB` 腰斩至 `465 MiB`**（消除了 `118.6 MiB` `VACUUM` 内存尖刺与多线程 Arena 爆炸）。
- 空闲态 POSIX `/dev/shm` 共享内存从 **`6.00 MiB` 收敛到 `12 kB`（`-99.8%`）**。
- `10 万` 文件全路径索引数据结构净重压至 **`111.73 B/file`**（含操作系统 RSS 开销仅 `152.86 B/file`），搜索延时稳居 **`1.27 ms`**。

### 4.2 可直接带走的 Linux 测量与微基准速查表

1. **看瞬时真账（区分私有与共享）**
   ```bash
   grep -E "^(VmHWM|VmRSS|RssAnon|RssFile|RssShmem|Threads)" /proc/$PID/status
   cat /proc/$PID/smaps_rollup
   ```
2. **排查内存换磁盘 I/O 的副作用（监控真实刷盘字节数）**
   ```bash
   cat /proc/$PID/io          # 关注 write_bytes (真实落盘) 与 wchar (系统调用写入)
   iostat -d -x 1             # 关注 await 与 %util
   vmstat 1                   # 关注 si / so (Swap 抖动) 与 bi / bo
   ```
3. **验证共享内存物理页是否真的还给了内核**
   ```bash
   ls -lh /dev/shm/soeasy*    # 注意：ls 显示的是逻辑文件长度，真实物理页必须看 RSS/PSS!
   grep -A 15 "/dev/shm/" /proc/$PID/smaps | grep -E "Rss:|Pss:|Shared"
   ```
4. **写 C++ 内存微基准的防欺骗技巧**：
   只靠手写 `sizeof()` 计算“理论内存”常常被 `malloc` 头部、内存对齐和容器空置 `capacity` 欺骗。严谨做法是：**先在测试前执行 `malloc_trim(0)` 清理底座 -> 读取 `VmRSS` -> 构造目标数据集 -> 再读一次 `VmRSS` 算差值（真实系统账）**，再和对象自身的 `ApproximateMemoryUsage()` 做交叉校验——正如本次我们观察到 `10.66 MiB`（理论净载）vs `14.58 MiB`（真实 OS RSS 增量），差额精准反映桶碎片与页对齐边界。

---

## 五、工程避坑总结（Checklist）

1. **不要对着 `top` 里的总 `RES` 盲改业务代码**：先看 `PSS` 和 `RssAnon`，区分究竟是 GUI 渲染的 `libLLVM.so` / 字体 `RssFile`，还是自己代码堆出来的 `RssAnon`。
2. **先盯过程峰值 `VmHWM`，再看稳态 `VmRSS`**：像 `VACUUM` 默认 `temp_store = MEMORY`、扫盘 `vector` 不 `.swap()` 缩容这类瞬时尖峰，稳态快照里看不出代码问题，全留在 `VmHWM` 和遗留 Arena 里。
3. **加密数据库慎开 `mmap`**：不透明密文页无法享受 OS 页缓存零拷贝，开大 `mmap` 往往只是虚增几百兆 `VmRSS`。
4. **任何限流参数都要防“写放大”反噬**：`AsyncWriteQueue` 不能靠无脑缩小 `batch_size` 降内存，要用 `queue_max_bytes` 控流，用 `batch_size >= 100` 保吞吐与磁盘寿命。
5. **在 Linux 上小心 `std::shared_mutex` 的调度倾向**：高并发读+偶发批量写场景，明确评估是否需要 `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` 防止写者饥饿。
