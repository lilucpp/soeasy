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

# 2. 准备分发目录并预先收集非编译资源
echo "[2/4] 准备分发目录与静态资源 (词典、模型、第三方依赖)..."
rm -rf "${STAGE_DIR}" "${ZIP_FILE}"
mkdir -p "${STAGE_DIR}/bin/platforms"
mkdir -p "${STAGE_DIR}/bin/imageformats"
mkdir -p "${STAGE_DIR}/dict"
mkdir -p "${STAGE_DIR}/models"

# 拷贝结巴分词词典、PP-OCRv6 模型与文档
cp -rp "${PROJECT_ROOT}/third_party/cppjieba/dict/"* "${STAGE_DIR}/dict/" 2>/dev/null || true
cp -rp "${PROJECT_ROOT}/models/ppocr_v6" "${STAGE_DIR}/models/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/README.md" "${STAGE_DIR}/" 2>/dev/null || true

# 拷贝第三方运行时库 (ONNX Runtime, DocToText)
cp -p "${PROJECT_ROOT}/third_party/doctotext/lib/"*.dll "${STAGE_DIR}/bin/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/third_party/onnxruntime/lib/"*.dll "${STAGE_DIR}/bin/" 2>/dev/null || true

# 3. 启动指定实例名的容器执行交叉编译并提取 MinGW 依赖库与插件
echo "[3/4] 启动容器 [${CONTAINER_NAME}] 执行编译与依赖收敛..."
MINGW_SYSROOT="/usr/x86_64-w64-mingw32/sys-root/mingw"
${DOCKER_RUN} run --name "${CONTAINER_NAME}" --rm -v "${PROJECT_ROOT}":/workspace -w /workspace "${IMAGE_NAME}" bash -c "
  set -euo pipefail
  mingw64-cmake -B build-win64 -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
  ninja -C build-win64 soeasy-gui soeasy-worker -j\$(nproc)

  cp -p build-win64/soeasy-gui.exe dist/${DIST_NAME}/bin/
  cp -p build-win64/soeasy-worker.exe dist/${DIST_NAME}/bin/
  cp -p ${MINGW_SYSROOT}/bin/Qt5Core.dll dist/${DIST_NAME}/bin/
  cp -p ${MINGW_SYSROOT}/bin/Qt5Gui.dll dist/${DIST_NAME}/bin/
  cp -p ${MINGW_SYSROOT}/bin/Qt5Widgets.dll dist/${DIST_NAME}/bin/
  cp -p ${MINGW_SYSROOT}/bin/libopencv_*.dll dist/${DIST_NAME}/bin/
  cp -p ${MINGW_SYSROOT}/bin/libuchardet*.dll dist/${DIST_NAME}/bin/
  cp -p ${MINGW_SYSROOT}/bin/iconv*.dll dist/${DIST_NAME}/bin/ 2>/dev/null || cp -p ${MINGW_SYSROOT}/bin/libiconv*.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/bin/libxml2*.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/bin/zlib1.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/bin/libgcc_s_seh-1.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/bin/libstdc++-6.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/bin/libwinpthread-1.dll dist/${DIST_NAME}/bin/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/lib/qt5/plugins/platforms/qwindows.dll dist/${DIST_NAME}/bin/platforms/
  cp -p ${MINGW_SYSROOT}/lib/qt5/plugins/imageformats/qjpeg.dll dist/${DIST_NAME}/bin/imageformats/ 2>/dev/null || true
  cp -p ${MINGW_SYSROOT}/lib/qt5/plugins/imageformats/qico.dll dist/${DIST_NAME}/bin/imageformats/ 2>/dev/null || true
"

# 4. 生成 Zip 压缩包
echo "[4/4] 压缩生成 Windows 发布包..."
(cd "${DIST_DIR}" && zip -r -q "${DIST_NAME}.zip" "${DIST_NAME}")

echo "========================================================"
echo "   Windows x64 打包完成: ${ZIP_FILE}"
echo "========================================================"
