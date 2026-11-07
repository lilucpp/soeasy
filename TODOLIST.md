# TODO

- [ ] 移除索引目录时联动删除对应的索引数据
- [ ] 压缩包格式（zip, tar, gz, 7z, rar, bz2, xz 等）内容的递归解压与正文全文检索解析
- [ ] 设置界面提供“自定义分词词典 / 新增分词”管理功能（配合底层 DAT+Overlay 双层分词架构，支持用户添加、删除自定义专有名词）
- [ ] Linux 监控注册阶段物理剪枝与监控库架构升级（防御 inotify 描述符耗尽与虚拟文件系统挂起）：
  - 调研与评估替代 efsw 的方案；
  - 方案 A：针对 efsw 维护/提供 Inotify 递归添加剪枝 patch（复用 IndexConfig 规则在向内核注册前直接跳过排除目录）；
  - 方案 B：基于 epoll + inotify / fsnotify 实现极轻量且支持剪枝的高性能 Linux 监控后端；
  - 方案 C：调研引入其他业界成熟监控库（如 fswatch 过滤后端）。
- [x] 优化结巴分词内存占用：引入 Darts-clone 双数组字典树 (DAT) 与 mmap 零拷贝加载，消除启动解析耗时与 120MB 动态堆内存消耗
- [x] 引入 reproc++ 现代进程生命周期治理，彻底消除 Linux 下 Worker 进程僵死 (<defunct>)，Linux 下切换为内核抽象套接字 IPC 杜绝磁盘残留
- [x] 优化 OCR 内存占用与解析性能：Header Probe 零拷贝尺寸嗅探 (<1μs)、双档位分辨率限边 (普通文档 736 / 内嵌图 640) 与 glibc 内存 Arena 碎片治理
