#!/usr/bin/env bash
# Copyright 2026 SoEasy Team. All rights reserved.
# One-click Windows x64 cross-compile and packaging script using Docker.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VERSION="${1:-${VERSION:-$(git -C "${PROJECT_ROOT}" describe --tags --abbrev=0 2>/dev/null | sed 's/^v//' || echo "0.2.0")}}"
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

# 2. 准备分发目录并预先收集静态资源
echo "[2/4] 准备分发目录与静态资源 (词典、模型、图标、文档)..."
rm -rf "${STAGE_DIR}" "${ZIP_FILE}"
mkdir -p "${STAGE_DIR}/bin/platforms"
mkdir -p "${STAGE_DIR}/bin/imageformats"
mkdir -p "${STAGE_DIR}/bin/styles"
mkdir -p "${STAGE_DIR}/dict"
mkdir -p "${STAGE_DIR}/models"
mkdir -p "${STAGE_DIR}/icons"

# 拷贝结巴分词词典、PP-OCRv6 模型、图标与文档
cp -rp "${PROJECT_ROOT}/third_party/cppjieba/dict/"* "${STAGE_DIR}/dict/" 2>/dev/null || true
cp -rp "${PROJECT_ROOT}/models/ppocr_v6" "${STAGE_DIR}/models/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/README.md" "${STAGE_DIR}/" 2>/dev/null || true
cp -rp "${PROJECT_ROOT}/resources/icons/"* "${STAGE_DIR}/icons/" 2>/dev/null || true

# 3. 启动指定实例名的容器执行交叉编译并提取全部运行时依赖库与插件
echo "[3/4] 启动容器 [${CONTAINER_NAME}] 执行编译与依赖收敛..."
MINGW_SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"
${DOCKER_RUN} run --name "${CONTAINER_NAME}" --rm -v "${PROJECT_ROOT}":/workspace -w /workspace "${IMAGE_NAME}" bash -c "
  set -euo pipefail
  mingw64-cmake -B build-win64 -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
  ninja -C build-win64 soeasy-gui soeasy-worker -j\$(nproc)

  echo '[3/4.1] 拷贝主程序与 Qt 平台及样式插件...'
  cp -p build-win64/soeasy-gui.exe dist/${DIST_NAME}/bin/
  cp -p build-win64/soeasy-worker.exe dist/${DIST_NAME}/bin/
  # 仅拷贝 DocToText 专用动态库，避免旧版 zlib1/libgcc 等系统库覆盖 MinGW 官方现代库
  cp -p third_party/doctotext/lib/doctotext.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p third_party/doctotext/lib/libwv2*.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p third_party/doctotext/lib/pthreadGC2.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p third_party/doctotext/lib/libiconv*.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p third_party/onnxruntime/lib/*.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/lib/qt5/plugins/platforms/qwindows.dll dist/${DIST_NAME}/bin/platforms/
  cp -p ${MINGW_SYSROOT}/lib/qt5/plugins/imageformats/*.dll dist/${DIST_NAME}/bin/imageformats/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/lib/qt5/plugins/styles/*.dll dist/${DIST_NAME}/bin/styles/ 2>/dev/null || true

  echo '[3/4.2] 执行自动化 PE 依赖递归解析与完整收敛 (收集 GUI & Worker 全部依赖库)...'
  python3 scripts/collect_win64_deps.py \
    --target-dir dist/${DIST_NAME}/bin \
    --search-dirs \
      ${MINGW_SYSROOT}/bin \
      third_party/doctotext/lib \
      third_party/onnxruntime/lib
"

# 创建根目录便捷启动脚本 run.bat
cat << 'EOF' > "${STAGE_DIR}/run.bat"
@echo off
set APP_DIR=%~dp0
start "" "%APP_DIR%bin\soeasy-gui.exe" %*
EOF

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
