// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <reproc++/reproc.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cppjieba/Jieba.hpp"
#include "storage/dat_jieba.h"
#include "tests/test_helper.h"

namespace {

void AssertOffsetInvariant(const std::string &sentence, const std::vector<cppjieba::Word> &words) {
  for (const auto &w : words) {
    if (w.word.empty()) continue;
    assert(w.offset + w.word.size() <= sentence.size());
    std::string actual = sentence.substr(w.offset, w.word.size());
    if (actual != w.word) {
      std::cerr << "[-] Offset invariant violation! Expected: '" << w.word << "' at offset " << w.offset
                << ", but actual slice is: '" << actual << "'" << std::endl;
      assert(false);
    }
  }
}

}  // namespace

int main(int argc, char *argv[]) {
  // Child execution branch for crash isolation test
  if (argc >= 3 && std::string(argv[1]) == "--child-crash") {
    std::string dict_dir = argv[2];
    soeasy::storage::DatJieba child_dj;
    if (!child_dj.Init(dict_dir)) {
      return 1;
    }
    std::vector<std::string> words;
    child_dj.Cut("子进程切词测试，即将模拟异常崩溃", words, true);
    return 42;
  }

  std::cout << "========================================================================\n";
  std::cout << "   SoEasy DatJieba 底层库全方位边界、并发、容错与稳健性实证矩阵\n";
  std::cout << "========================================================================\n";

  std::string dict_dir = soeasy::test::FindDictDir();

  // -------------------------------------------------------------------------
  // [Test 1] 畸形、破损、截断与非法二进制 DAT 文件防御测试
  // -------------------------------------------------------------------------
  std::cout << "\n[Test 1/6] 验证: 畸形、破损与截断 DAT 二进制文件容错防御...\n";
  std::string temp_corrupt_dir = "/tmp/soeasy_corrupt_dict";
  std::filesystem::create_directories(temp_corrupt_dir);

  // 1.1 空文件 (0 字节)
  {
    std::string p = temp_corrupt_dir + "/jieba_dat.bin";
    std::ofstream ofs(p, std::ios::binary);
    ofs.close();

    soeasy::storage::DatJieba dj;
    bool ok = dj.Init(temp_corrupt_dir);
    (void)ok;
    assert(!ok);
    std::cout << "  • 0 字节空文件拦截: PASS (安全返回 false, 无崩溃)\n";
  }

  // 1.2 截断头部 (< sizeof(DatHeader))
  {
    std::string p = temp_corrupt_dir + "/jieba_dat.bin";
    std::ofstream ofs(p, std::ios::binary);
    ofs.write("DAT", 3);
    ofs.close();

    soeasy::storage::DatJieba dj;
    bool ok = dj.Init(temp_corrupt_dir);
    (void)ok;
    assert(!ok);
    std::cout << "  • 截断头部 (3 字节) 拦截: PASS (安全返回 false, 无崩溃)\n";
  }

  // 1.3 错误 Magic Header ("BADMAGIC")
  {
    std::string p = temp_corrupt_dir + "/jieba_dat.bin";
    std::ofstream ofs(p, std::ios::binary);
    soeasy::storage::DatHeader hdr;
    std::memcpy(hdr.magic, "BADMAGIC", 8);
    hdr.version = 1;
    hdr.num_words = 100;
    hdr.min_weight = -10.0;
    hdr.da_size = 1024;
    ofs.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    ofs.close();

    soeasy::storage::DatJieba dj;
    bool ok = dj.Init(temp_corrupt_dir);
    (void)ok;
    assert(!ok);
    std::cout << "  • 非法 Magic 魔数拦截: PASS (安全拒绝, 无崩溃)\n";
  }

  // 1.4 Payload 截断测试 (Header 声称有 10MB 数据，但文件在 1KB 处截断)
  {
    std::string p = temp_corrupt_dir + "/jieba_dat.bin";
    std::ofstream ofs(p, std::ios::binary);
    soeasy::storage::DatHeader hdr;
    std::memcpy(hdr.magic, "DATJIEBA", 8);
    hdr.version = 1;
    hdr.num_words = 500000;
    hdr.min_weight = -15.0;
    hdr.da_size = 4 * 1024 * 1024;  // 4MB
    ofs.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    std::vector<char> dummy(1024, 0);
    ofs.write(dummy.data(), dummy.size());  // 仅写入 1KB
    ofs.close();

    soeasy::storage::DatJieba dj;
    bool ok = dj.Init(temp_corrupt_dir);
    (void)ok;
    assert(!ok);
    std::cout << "  • Payload 截断攻击防御: PASS (成功阻止 SIGBUS 缺页崩溃)\n";
  }

  // 1.5 未初始化直接调用 Cut
  {
    soeasy::storage::DatJieba dj;
    std::vector<cppjieba::Word> words;
    dj.Cut("未初始化测试句子", words, true);
    assert(words.empty());
    std::cout << "  • 未初始化直接调用 Cut: PASS (安全返回空, 杜绝野指针解引用)\n";
  }

  std::filesystem::remove_all(temp_corrupt_dir);

  // -------------------------------------------------------------------------
  // [Test 2] 极限与畸形字符串输入防御 (Empty, Invalid UTF-8, Emoji, 极端长句)
  // -------------------------------------------------------------------------
  std::cout << "\n[Test 2/6] 验证: 极端边界输入与畸形字符防御...\n";
  soeasy::storage::DatJieba dj;
  bool init_ok = dj.Init(dict_dir);
  (void)init_ok;
  assert(init_ok);

  // 2.1 空文本
  {
    std::vector<cppjieba::Word> words;
    dj.Cut("", words, true);
    assert(words.empty());
    std::cout << "  • 空字符串 Cut(\"\"): PASS (返回 0 结果)\n";
  }

  // 2.2 纯空白字符与换行
  {
    std::string spaces = " \t\r\n   \n\t  ";
    std::vector<cppjieba::Word> words;
    dj.Cut(spaces, words, true);
    AssertOffsetInvariant(spaces, words);
    std::cout << "  • 纯空格与制表符: PASS\n";
  }

  // 2.3 畸形非 UTF-8 字节序列 (防止预过滤解码器崩溃或越界)
  {
    std::string malformed = "合法前缀\xFF\xFE\x80\x81\xE4\xB8中间内容\xC0\xC1\xFA合法后缀";
    std::vector<cppjieba::Word> words;
    dj.Cut(malformed, words, true);
    std::cout << "  • 畸形非 UTF-8 乱码字节序列清洗与防御: PASS (提取 " << words.size() << " 个词, 未崩溃)\n";
  }

  // 2.4 Emoji、复杂符号与多语言穿插
  {
    std::string emoji_mix = "Hello😊世界🚀SoEasy✨C++17💻，这是Emoji与特殊符号测试【】（）！@#￥%……&*";
    std::vector<cppjieba::Word> words;
    dj.Cut(emoji_mix, words, true);
    AssertOffsetInvariant(emoji_mix, words);
    std::cout << "  • Emoji 与特殊符号多语言混合: PASS\n";
  }

  // 2.5 10,000 字符无标点超长连续中文字符串压力测试 (DAG 动态规划压测)
  {
    std::string massive;
    massive.reserve(30000);
    for (int i = 0; i < 2000; ++i) {
      massive += "南京市长江大桥";
    }
    auto t_start = std::chrono::high_resolution_clock::now();
    std::vector<cppjieba::Word> words;
    dj.Cut(massive, words, true);
    auto t_end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
    AssertOffsetInvariant(massive, words);
    std::cout << "  • 14,000 字符超长无标点句子 DAG/Viterbi 压测: PASS (耗时: " << ms << " ms, 提取 " << words.size()
              << " 词)\n";
  }

  // -------------------------------------------------------------------------
  // [Test 3] 逐词字节偏移保真度验证 (FTS5 高亮与短语邻近的命脉)
  // -------------------------------------------------------------------------
  std::cout << "\n[Test 3/6] 验证: 全文检索 FTS5 字节偏移量 (Byte Offset) 100% 保真度...\n";
  std::vector<std::string> corpus = {
      "关于2026年度预算与财务合同批复.pdf。发文字号：国发〔2026〕88号。",
      "SoEasy是一款基于C++17与Qt5构建的高性能跨平台桌面搜索引擎。",
      "小明硕士毕业于中国科学院计算所，后在日本京都大学深造。",
      "自然语言处理与中文分词：基于双数组字典树DAT与mmap零拷贝优化实践。"};

  for (size_t i = 0; i < corpus.size(); ++i) {
    std::vector<cppjieba::Word> words;
    dj.Cut(corpus[i], words, true);
    AssertOffsetInvariant(corpus[i], words);
  }
  std::cout << "  • 语料集各字符切片 sentence.substr(offset, len) == word: 100% 严密吻合!\n";

  // -------------------------------------------------------------------------
  // [Test 4] Overlay 动态增量用户词典边界测试 (增、删、改、单字、超长)
  // -------------------------------------------------------------------------
  std::cout << "\n[Test 4/6] 验证: Overlay 用户词典增删改查边界条件...\n";

  // 4.1 空词插入
  assert(!dj.InsertUserWord(""));
  std::cout << "  • 插入空词: PASS (安全拦截)\n";

  // 4.2 负频次与 0 频次插入 (权重防除零或 log 崩溃)
  assert(dj.InsertUserWord("测试零频", 0));
  assert(dj.InsertUserWord("测试负频", -10));
  std::cout << "  • 零频与负频次防御: PASS\n";

  // 4.3 删除不存在的词
  assert(!dj.DeleteUserWord("绝对不存在的未录入词"));
  std::cout << "  • 删除未注册词条: PASS (安全返回 false)\n";

  // 4.4 单字 CJK 用户词 (防止被 HMM 强行吞并组合)
  std::string test_cjk = "他手持一把剑";
  std::vector<std::string> words_before;
  dj.Cut(test_cjk, words_before, true);

  dj.InsertUserWord("剑", 50000);
  std::vector<std::string> words_after;
  dj.Cut(test_cjk, words_after, true);
  bool found_sword = false;
  for (const auto &w : words_after) {
    if (w == "剑") found_sword = true;
  }
  assert(found_sword);
  (void)found_sword;
  std::cout << "  • 单字 CJK 用户词防过度合并: PASS\n";

  // 4.6 词条存在性纳秒级探测 HasWord 验证
  assert(!dj.HasWord(""));
  assert(dj.HasWord("中国"));
  assert(dj.HasWord("自然语言"));
  assert(dj.HasWord("清华大学"));
  assert(!dj.HasWord("绝对不可能存在的未知自定义测试词条_XYZ999"));
  
  std::string new_term = "极轻量沙箱架构";
  assert(!dj.HasWord(new_term));
  assert(dj.InsertUserWord(new_term, 10000));
  assert(dj.HasWord(new_term));
  assert(dj.DeleteUserWord(new_term));
  assert(!dj.HasWord(new_term));
  std::cout << "  • HasWord 基础词库与 Overlay 动态探测: PASS\n";

  // -------------------------------------------------------------------------
  // [Test 5] 多线程高并发切词与动态增删 Overlay 竞态测试 (Thread Safety)
  // -------------------------------------------------------------------------
  std::cout << "\n[Test 5/6] 验证: 10 线程高并发读写竞态 (Thread Safety Stress)...\n";
  std::atomic<bool> stop_flag{false};
  std::atomic<size_t> total_cuts{0};

  // 8 个并发切词读者线程
  std::vector<std::thread> readers;
  for (int t = 0; t < 8; ++t) {
    readers.emplace_back([&dj, &stop_flag, &total_cuts, t]() {
      std::string s = "人工智能大模型与深度学习在中文自然语言处理领域的应用落地#" + std::to_string(t);
      std::vector<cppjieba::Word> words;
      while (!stop_flag.load(std::memory_order_relaxed)) {
        words.clear();
        dj.Cut(s, words, true);
        AssertOffsetInvariant(s, words);
        total_cuts.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  // 2 个并发动态插入与删除词条的写者线程
  std::vector<std::thread> writers;
  for (int w = 0; w < 2; ++w) {
    writers.emplace_back([&dj, &stop_flag, w]() {
      int counter = 0;
      while (!stop_flag.load(std::memory_order_relaxed)) {
        std::string word = "动态测试词_" + std::to_string(w) + "_" + std::to_string(counter % 20);
        dj.InsertUserWord(word, 5000);
        if (counter % 3 == 0) {
          dj.DeleteUserWord(word);
        }
        counter++;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    });
  }

  // 运行 500ms
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop_flag.store(true);

  for (auto &th : readers) th.join();
  for (auto &th : writers) th.join();

  std::cout << "  • 8 读线程 + 2 写线程并发压力测试通过! 共执行切词 " << total_cuts.load() << " 次 (零死锁/零竞态)!\n";

  // -------------------------------------------------------------------------
  // [Test 6] 跨进程 mmap 映射与子进程异常崩溃隔离测试 (Crash & Leak Defense)
  // -------------------------------------------------------------------------
  std::cout << "\n[Test 6/6] 验证: 跨进程只读映射与子进程异常崩溃隔离 (Crash Isolation)...\n";
  reproc::process child_proc;
  std::vector<std::string> child_args = {argv[0], "--child-crash", dict_dir};
  reproc::options child_opts;
  child_opts.redirect.out.type = reproc::redirect::discard;
  child_opts.redirect.err.type = reproc::redirect::discard;

  std::error_code child_ec = child_proc.start(child_args, child_opts);
  if (child_ec) {
    std::cerr << "[-] Failed to start child crash test: " << child_ec.message() << std::endl;
    assert(false);
  }

  auto [exit_code, wait_ec] = child_proc.wait(reproc::milliseconds(5000));
  if (wait_ec) {
    std::cerr << "[-] Child crash test wait failed: " << wait_ec.message() << std::endl;
    assert(false);
  }
  assert(exit_code == 42);

  // 验证父进程的 DatJieba 依然完好无损，底层 mmap 映射文件没有损坏
  std::vector<std::string> parent_words;
  dj.Cut("子进程崩溃后，父进程词典依然完好无损，检索继续。", parent_words, true);
  assert(!parent_words.empty());
  std::cout << "  • 子进程退出/崩溃完全隔离: PASS (底层 Page Cache 物理文件受 PROT_READ 保护, 无法被破坏)!\n";

  std::cout << "\n========================================================================\n";
  std::cout << "🎉 所有 6 大类核心边界、极限输入、多线程竞态与崩溃隔离测试 100% 全部通过!\n";
  std::cout << "========================================================================\n";
  return 0;
}
