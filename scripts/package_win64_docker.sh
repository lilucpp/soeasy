#!/usr/bin/env bash
# Copyright 2026 SoEasy Team. All rights reserved.
# One-click Windows x64 cross-compile and packaging script using Docker.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

RAW_TAG="$(git -C "${PROJECT_ROOT}" describe --tags --match 'v*' --abbrev=0 2>/dev/null || true)"
VERSION="${1:-${VERSION:-${RAW_TAG#v}}}"
VERSION="${VERSION:-0.4.6}"
PLATFORM="windows-x64"
DIST_NAME="soeasy-v${VERSION}-${PLATFORM}"
DIST_DIR="${PROJECT_ROOT}/dist"
STAGE_DIR="${DIST_DIR}/${DIST_NAME}"
ZIP_FILE="${DIST_DIR}/${DIST_NAME}.zip"
IMAGE_NAME="soeasy-win64-cross"
CONTAINER_NAME="soeasy-win64-builder"

# 自动适配 Docker 组权限
DOCKER_RUN="docker"
if ! docker ps >/dev/null 2>&1; then
    if sg docker -c "docker ps" >/dev/null 2>&1; then
        DOCKER_RUN="sg docker -c docker"
    fi
fi

# 确保退出或中断时清理同名容器实例
cleanup() {
    ${DOCKER_RUN} rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
cleanup

echo "========================================================"
echo "   SoEasy 桌面搜索引擎 Windows x64 交叉编译打包系统"
echo "   版本: ${VERSION}  |  平台: ${PLATFORM}"
echo "========================================================"

# 1. 确保 Docker 镜像存在
if ! ${DOCKER_RUN} image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
    echo "[1/4] 未检测到镜像 ${IMAGE_NAME}，正在从 Dockerfile.win64 构建..."
    ${DOCKER_RUN} build -f "${PROJECT_ROOT}/Dockerfile.win64" -t "${IMAGE_NAME}" "${PROJECT_ROOT}"
else
    echo "[1/4] 检测到已存在镜像: ${IMAGE_NAME}"
fi

# 确保 jieba_dat.bin 双数组字典预编译完成
if [ ! -f "${PROJECT_ROOT}/third_party/cppjieba/dict/jieba_dat.bin" ]; then
    echo "[Dict] 宿主机正在预生成 jieba_dat.bin 双数组字典树..."
    if [ ! -f "${PROJECT_ROOT}/build/build_dat_dict" ]; then
        cmake -B "${PROJECT_ROOT}/build" -S "${PROJECT_ROOT}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
        cmake --build "${PROJECT_ROOT}/build" --target build_dat_dict -j$(nproc 2>/dev/null || echo 4)
    fi
    "${PROJECT_ROOT}/build/build_dat_dict" "${PROJECT_ROOT}/third_party/cppjieba/dict/jieba.dict.utf8" "${PROJECT_ROOT}/third_party/cppjieba/dict/jieba_dat.bin"
fi

# 2. 准备分发目录
echo "[2/4] 准备分发目录..."
rm -rf "${STAGE_DIR}" "${ZIP_FILE}"
mkdir -p "${STAGE_DIR}"

# 3. 启动指定实例名的容器执行交叉编译并调用共享 staging 脚本收敛依赖
echo "[3/4] 启动容器 [${CONTAINER_NAME}] 执行编译与依赖收敛..."
MINGW_SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"
${DOCKER_RUN} run --name "${CONTAINER_NAME}" --rm -v "${PROJECT_ROOT}":/workspace -w /workspace "${IMAGE_NAME}" bash -c "
  set -euo pipefail
  mingw64-cmake -B build-win64 -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
  ninja -C build-win64 soeasy-gui soeasy-worker soeasy-cli -j\$(nproc)
  bash scripts/package_win64_stage.sh \
    --build-dir build-win64 \
    --stage-dir dist/${DIST_NAME} \
    --mingw-bin ${MINGW_SYSROOT}/bin \
    --qt-dir ${MINGW_SYSROOT}
"

# 4. 生成 Zip 压缩包
echo "[4/4] 压缩生成 Windows 发布包..."
(cd "${DIST_DIR}" && zip -r -q "${DIST_NAME}.zip" "${DIST_NAME}")

ZIP_SIZE=$(du -h "${ZIP_FILE}" | cut -f1)
SHA256=$(sha256sum "${ZIP_FILE}" | cut -d' ' -f1)

echo "========================================================"
echo "🎉 Windows x64 打包完成!"
echo "📦 输出文件: ${ZIP_FILE}"
echo "📊 文件大小: ${ZIP_SIZE}"
echo "🔒 SHA256:  ${SHA256}"
echo "========================================================"
