#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "service/name_index.h"

namespace soeasy {
namespace service {
namespace {

TEST(NameIndexTest, PathNormalizationAndExactRoundTrip) {
  NameIndex index;

  std::vector<NameRecord> inputs = {
      {1, "Report.PDF", "/data/docs/Report.PDF", ".pdf", 1024, 100},
      {2, "root.cfg", "/root.cfg", ".cfg", 64, 200},
      {3, "notes.txt", R"(C:\Users\Alice\notes.txt)", ".txt", 256, 300},
      {4, "plain.log", "plain.log", ".log", 512, 400},
      {5, "double.txt", "/a//double.txt", ".txt", 12, 500},
      {6, "slash_double.txt", "//slash_double.txt", ".txt", 34, 600},
      {7, "", "/trailing_dir/", "", 0, 700},
  };

  for (const auto &rec : inputs) {
    index.Upsert(rec);
  }
  EXPECT_EQ(index.size(), inputs.size());

  // Query each via prefix "/" or full search to check exact reconstructed normalized paths.
  int total = 0;
  auto hits = index.Search(".", 50, 0, "", &total);
  EXPECT_EQ(total, 6);

  auto find_by_id = [](const std::vector<NameMatchResult> &res, int64_t id) -> const NameMatchResult * {
    for (const auto &r : res) {
      if (r.doc_id == id) return &r;
    }
    return nullptr;
  };

  const auto *r1 = find_by_id(hits, 1);
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(r1->file_path, "/data/docs/Report.PDF");
  EXPECT_EQ(r1->file_name, "Report.PDF");
  EXPECT_EQ(r1->ext, ".pdf");
  EXPECT_EQ(r1->file_size, 1024u);
  EXPECT_EQ(r1->mtime, 100);

  const auto *r2 = find_by_id(hits, 2);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(r2->file_path, "/root.cfg");

  const auto *r3 = find_by_id(hits, 3);
  ASSERT_NE(r3, nullptr);
  EXPECT_EQ(r3->file_path, "C:/Users/Alice/notes.txt");

  const auto *r4 = find_by_id(hits, 4);
  ASSERT_NE(r4, nullptr);
  EXPECT_EQ(r4->file_path, "plain.log");

  const auto *r5 = find_by_id(hits, 5);
  ASSERT_NE(r5, nullptr);
  EXPECT_EQ(r5->file_path, "/a//double.txt");

  const auto *r6 = find_by_id(hits, 6);
  ASSERT_NE(r6, nullptr);
  EXPECT_EQ(r6->file_path, "//slash_double.txt");

  // Prefix search finds trailing_dir entry too
  auto dir_hits = index.Search("", 10, 0, "/trailing_dir", &total);
  ASSERT_EQ(total, 1);
  EXPECT_EQ(dir_hits[0].doc_id, 7);
  EXPECT_EQ(dir_hits[0].file_path, "/trailing_dir/");
}

TEST(NameIndexTest, UpsertReplacementsAndSpecialDocIds) {
  NameIndex index;

  // Special doc_ids: 0, -1, INT64_MIN, INT64_MAX
  index.Upsert({0, "zero.txt", "/dir/zero.txt", ".txt", 10, 1});
  index.Upsert({-1, "neg.txt", "/dir/neg.txt", ".txt", 20, 2});
  index.Upsert({std::numeric_limits<int64_t>::min(), "min.txt", "/dir/min.txt", ".txt", 30, 3});
  index.Upsert({std::numeric_limits<int64_t>::max(), "max.txt", "/dir/max.txt", ".txt", 40, 4});
  EXPECT_EQ(index.size(), 4u);

  uint64_t fsize = 0;
  int64_t mtime = 0;
  ASSERT_TRUE(index.GetMetadata(0, &fsize, &mtime));
  EXPECT_EQ(fsize, 10u);
  ASSERT_TRUE(index.GetMetadata(std::numeric_limits<int64_t>::min(), &fsize, &mtime));
  EXPECT_EQ(fsize, 30u);

  // Move doc_id 0 to a new path; old path must no longer resolve to doc_id 0.
  index.Upsert({0, "moved.md", "/other/moved.md", ".md", 99, 9});
  EXPECT_EQ(index.size(), 4u);
  ASSERT_TRUE(index.GetMetadata(0, &fsize, &mtime));
  EXPECT_EQ(fsize, 99u);
  EXPECT_EQ(mtime, 9);

  EXPECT_TRUE(index.Search("zero.txt").empty());
  auto moved_hits = index.Search("moved.md");
  ASSERT_EQ(moved_hits.size(), 1u);
  EXPECT_EQ(moved_hits[0].doc_id, 0);
  EXPECT_EQ(moved_hits[0].file_path, "/other/moved.md");
  EXPECT_EQ(moved_hits[0].ext, ".md");

  index.RemoveByPath("/dir/zero.txt");  // Must be no-op since doc_id 0 moved
  EXPECT_EQ(index.size(), 4u);

  // Upsert a different doc_id onto an occupied path evicts old doc_id
  index.Upsert({500, "moved.md", "/other/moved.md", ".md", 777, 77});
  EXPECT_EQ(index.size(), 4u);
  EXPECT_FALSE(index.GetMetadata(0, &fsize, &mtime));
  ASSERT_TRUE(index.GetMetadata(500, &fsize, &mtime));
  EXPECT_EQ(fsize, 777u);

  // Custom file_name differing from path basename
  index.Upsert({600, "CustomAlias.doc", "/hidden/raw_name.bin", ".doc", 123, 45});
  auto alias_hits = index.Search("customalias");
  ASSERT_EQ(alias_hits.size(), 1u);
  EXPECT_EQ(alias_hits[0].doc_id, 600);
  EXPECT_EQ(alias_hits[0].file_name, "CustomAlias.doc");
  EXPECT_EQ(alias_hits[0].file_path, "/hidden/raw_name.bin");
}

TEST(NameIndexTest, SearchScoringPaginationAndPrefixBoundary) {
  NameIndex index;

  // Scores:
  // 1: exact "report" -> 100.0
  // 2: starts with "report" -> 80.0 (mtime 200)
  // 3: starts with "report" -> 80.0 (mtime 500) -> ranks ahead of #2
  // 4: pos 3 ("my_report.pdf") -> 47.0
  // 5: far pos (pos 35) -> 50.0 - 30.0 = 20.0
  index.Upsert({1, "Report", "/work/docs/Report", "", 10, 100});
  index.Upsert({2, "report_old.txt", "/work/docs/report_old.txt", ".txt", 10, 200});
  index.Upsert({3, "report_new.txt", R"(\work\docs\sub\report_new.txt)", ".txt", 10, 500});
  index.Upsert({4, "my_report.pdf", "/work/docs_backup/my_report.pdf", ".pdf", 10, 300});
  index.Upsert({5, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaareport.txt",
                "/other/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaareport.txt", ".txt", 10, 999});

  // Normalize leading backslash in #3: note R"(\work\...)" -> "/work/docs/sub/report_new.txt"
  int total = 0;
  auto empty_both = index.Search("", 10, 0, "", &total);
  EXPECT_TRUE(empty_both.empty());
  EXPECT_EQ(total, 0);

  auto ranked = index.Search("REPORT", 10, 0, "", &total);
  ASSERT_EQ(total, 5);
  ASSERT_EQ(ranked.size(), 5u);
  EXPECT_EQ(ranked[0].doc_id, 1);
  EXPECT_DOUBLE_EQ(ranked[0].score, 100.0);
  EXPECT_EQ(ranked[1].doc_id, 3);
  EXPECT_DOUBLE_EQ(ranked[1].score, 80.0);
  EXPECT_EQ(ranked[2].doc_id, 2);
  EXPECT_DOUBLE_EQ(ranked[2].score, 80.0);
  EXPECT_EQ(ranked[3].doc_id, 4);
  EXPECT_DOUBLE_EQ(ranked[3].score, 47.0);
  EXPECT_EQ(ranked[4].doc_id, 5);
  EXPECT_DOUBLE_EQ(ranked[4].score, 20.0);

  // Pagination checks
  auto page2 = index.Search("report", 2, 1, "", &total);
  EXPECT_EQ(total, 5);
  ASSERT_EQ(page2.size(), 2u);
  EXPECT_EQ(page2[0].doc_id, 3);
  EXPECT_EQ(page2[1].doc_id, 2);

  EXPECT_TRUE(index.Search("report", 0, 0, "", &total).empty());
  EXPECT_EQ(total, 5);
  EXPECT_TRUE(index.Search("report", 10, 5, "", &total).empty());
  EXPECT_EQ(total, 5);
  auto neg_offset = index.Search("report", 1, -3, "", &total);
  ASSERT_EQ(neg_offset.size(), 1u);
  EXPECT_EQ(neg_offset[0].doc_id, 1);

  // Path prefix checks:
  // "/work/docs" matches both "/work/docs/*" and "/work/docs_backup/*" (StartsWith behavior)
  auto prefix_no_slash = index.Search("", 10, 0, "/work/docs", &total);
  EXPECT_EQ(total, 4);
  for (const auto &r : prefix_no_slash) {
    EXPECT_DOUBLE_EQ(r.score, 1.0);
  }

  // "/work/docs/" matches ONLY children under /work/docs/
  auto prefix_with_slash = index.Search("", 10, 0, R"(\work\docs\)", &total);
  EXPECT_EQ(total, 3);

  // Partial segment prefix "/work/do" matches all 4 /work/docs* files
  auto prefix_partial = index.Search("report", 10, 0, "/work/do", &total);
  EXPECT_EQ(total, 4);
}

TEST(NameIndexTest, RemoveDirectoryPrefixAndCompaction) {
  NameIndex index;

  // Exact path equal to directory prefix + nested descendants + sibling with shared prefix
  index.Upsert({1, "docs", "/data/docs", "", 0, 1});
  index.Upsert({2, "a.txt", "/data/docs/a.txt", ".txt", 10, 2});
  index.Upsert({3, "b.txt", "/data/docs/sub/b.txt", ".txt", 10, 3});
  index.Upsert({4, "keep.txt", "/data/docs_backup/keep.txt", ".txt", 10, 4});
  index.Upsert({5, "win.txt", R"(C:\work\proj\win.txt)", ".txt", 10, 5});
  index.Upsert({6, "win_keep.txt", R"(C:\work\proj2\win_keep.txt)", ".txt", 10, 6});

  index.RemoveDirectoryPrefix("/data/docs/");
  EXPECT_EQ(index.size(), 3u);
  uint64_t sz = 0;
  int64_t mt = 0;
  EXPECT_FALSE(index.GetMetadata(1, &sz, &mt));
  EXPECT_FALSE(index.GetMetadata(2, &sz, &mt));
  EXPECT_FALSE(index.GetMetadata(3, &sz, &mt));
  EXPECT_TRUE(index.GetMetadata(4, &sz, &mt));

  index.RemoveDirectoryPrefix(R"(C:\work\proj)");
  EXPECT_EQ(index.size(), 2u);
  EXPECT_FALSE(index.GetMetadata(5, &sz, &mt));
  EXPECT_TRUE(index.GetMetadata(6, &sz, &mt));

  // Mass insert + batch delete triggers automatic Compact() and shrinks RAM
  for (int64_t i = 100; i < 2100; ++i) {
    index.Upsert({i, "bulk_file_" + std::to_string(i) + ".dat",
                  "/bulk/dir_" + std::to_string(i % 50) + "/bulk_file_" + std::to_string(i) + ".dat", ".dat",
                  static_cast<uint64_t>(i), i});
  }
  EXPECT_EQ(index.size(), 2002u);
  const size_t peak_bytes = index.ApproximateMemoryUsage();

  std::vector<int64_t> to_remove;
  to_remove.reserve(1800);
  for (int64_t i = 100; i < 1900; ++i) {
    to_remove.push_back(i);
  }
  index.RemoveBatch(to_remove);
  EXPECT_EQ(index.size(), 202u);
  const size_t compacted_bytes = index.ApproximateMemoryUsage();
  EXPECT_LT(compacted_bytes, peak_bytes / 2);

  int total = 0;
  auto post_compact_hits = index.Search("bulk_file_2099", 10, 0, "/bulk/dir_49/", &total);
  ASSERT_EQ(total, 1);
  ASSERT_EQ(post_compact_hits.size(), 1u);
  EXPECT_EQ(post_compact_hits[0].doc_id, 2099);
  EXPECT_EQ(post_compact_hits[0].file_path, "/bulk/dir_49/bulk_file_2099.dat");

  index.Clear();
  EXPECT_EQ(index.size(), 0u);
  EXPECT_TRUE(index.Search("keep").empty());
}

TEST(NameIndexTest, ConcurrentReadersAndWriter) {
  NameIndex index;
  for (int64_t i = 1; i <= 500; ++i) {
    index.Upsert({i, "seed_" + std::to_string(i) + ".txt", "/seed/dir/seed_" + std::to_string(i) + ".txt",
                  ".txt", 100, i});
  }

  std::atomic<bool> stop{false};
  std::thread writer([&]() {
    for (int64_t round = 0; round < 300; ++round) {
      const int64_t id = 1000 + (round % 200);
      index.Upsert({id, "dyn_" + std::to_string(id) + ".log",
                    "/dyn/sub_" + std::to_string(round % 10) + "/dyn_" + std::to_string(id) + ".log", ".log",
                    static_cast<uint64_t>(round), round});
      if (round % 25 == 0) {
        index.Remove(1000 + ((round + 50) % 200));
      }
    }
    stop.store(true, std::memory_order_release);
  });

  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_acquire)) {
        auto res = index.Search("seed_42", 10, 0, "/seed");
        EXPECT_GE(res.size(), 1u);
        uint64_t sz = 0;
        int64_t mt = 0;
        EXPECT_TRUE(index.GetMetadata(1, &sz, &mt));
      }
    });
  }

  writer.join();
  for (auto &r : readers) {
    r.join();
  }
  EXPECT_GT(index.size(), 500u);
}

TEST(NameIndexTest, Scale100kFootprintAndSearchLatency) {
  NameIndex index;
  constexpr int kCount = 100000;
  constexpr int kDirs = 2000;
  const char *exts[] = {".cpp", ".h", ".md", ".pdf", ".json"};

  const auto build_start = std::chrono::steady_clock::now();
  for (int i = 0; i < kCount; ++i) {
    const int d = i % kDirs;
    const std::string ext = exts[i % 5];
    const std::string fname = (i == 98765) ? "TargetQuarterlyBudget2026.pdf"
                                           : ("project_module_file_" + std::to_string(i) + ext);
    const std::string path = "/home/user/workspace/project_" + std::to_string(d / 20) + "/sub_" +
                             std::to_string(d % 20) + "/" + fname;
    index.Upsert({static_cast<int64_t>(i + 1), fname, path, ext, static_cast<uint64_t>(i * 128), i});
  }
  const auto build_end = std::chrono::steady_clock::now();
  const double build_ms = std::chrono::duration<double, std::milli>(build_end - build_start).count();

  ASSERT_EQ(index.size(), static_cast<size_t>(kCount));

  const size_t approx_bytes = index.ApproximateMemoryUsage();
  const double bytes_per_file = static_cast<double>(approx_bytes) / kCount;
  const double mib = static_cast<double>(approx_bytes) / (1024.0 * 1024.0);

  // Measure 50 substring searches across 100k records
  const auto search_start = std::chrono::steady_clock::now();
  int total_hits = 0;
  for (int q = 0; q < 50; ++q) {
    auto res = index.Search(q == 0 ? "TargetQuarterlyBudget2026" : "module_file_999", 50, 0, "", &total_hits);
    ASSERT_FALSE(res.empty());
  }
  const auto search_end = std::chrono::steady_clock::now();
  const double avg_search_ms = std::chrono::duration<double, std::milli>(search_end - search_start).count() / 50.0;

  std::cout << std::fixed << std::setprecision(2) << "[NameIndex 100k Benchmark] build=" << build_ms
            << " ms, mem=" << mib << " MiB (" << bytes_per_file << " B/file), avg_search=" << avg_search_ms
            << " ms\n";

  // Verify the SoA + Arena layout stays comfortably compact (< 120 B/file including 29B filenames & hash tables)
  EXPECT_LT(bytes_per_file, 120.0);

  int total = 0;
  auto needle = index.Search("quarterlybudget", 10, 0, "/home/user/workspace/", &total);
  ASSERT_EQ(total, 1);
  ASSERT_EQ(needle.size(), 1u);
  EXPECT_EQ(needle[0].doc_id, 98766);
  EXPECT_EQ(needle[0].file_name, "TargetQuarterlyBudget2026.pdf");
}

}  // namespace
}  // namespace service
}  // namespace soeasy
