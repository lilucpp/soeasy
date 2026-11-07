#!/usr/bin/env bash
# Copyright 2026 SoEasy Team. All rights reserved.
# Use of this source code is governed by an Apache-2.0 license.
#
# 制作 / 预编译 jieba_dat.bin 双数组字典树二进制工具脚本

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

INPUT_DICT="${1:-${PROJECT_ROOT}/third_party/cppjieba/dict/jieba.dict.utf8}"
OUTPUT_DAT="${2:-${PROJECT_ROOT}/third_party/cppjieba/dict/jieba_dat.bin}"
BUILD_TOOL="${PROJECT_ROOT}/build/build_dat_dict"

echo "========================================================"
echo "   SoEasy 双数组字典树 (DAT) 二进制词典制作工具"
echo "========================================================"
echo "输入明文词典: ${INPUT_DICT}"
echo "输出二进制库: ${OUTPUT_DAT}"

if [ ! -f "${INPUT_DICT}" ]; then
  echo "[-] 错误: 输入词典文件不存在: ${INPUT_DICT}"
  exit 1
fi

# 若工具尚未编译，则先编译 build_dat_dict
if [ ! -f "${BUILD_TOOL}" ]; then
  echo "[1/2] 正在编译制作工具 build_dat_dict..."
  cmake -B "${PROJECT_ROOT}/build" -S "${PROJECT_ROOT}"
  cmake --build "${PROJECT_ROOT}/build" --target build_dat_dict -j$(nproc)
fi

echo "[2/2] 开始压制二进制双数组字典..."
"${BUILD_TOOL}" "${INPUT_DICT}" "${OUTPUT_DAT}"

echo "========================================================"
echo "🎉 制作完成!"
ls -lh "${OUTPUT_DAT}"
echo "========================================================"
