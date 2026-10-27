#!/usr/bin/env bash
# Copyright 2026 SoEasy Team. All rights reserved.
# One-click packaging script for SoEasy Desktop Search Engine.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VERSION="1.0.0"
PLATFORM="linux-x86_64"
DIST_NAME="soeasy-v${VERSION}-${PLATFORM}"
BUILD_DIR="${PROJECT_ROOT}/build"
DIST_DIR="${PROJECT_ROOT}/dist"
STAGE_DIR="${DIST_DIR}/${DIST_NAME}"
ZIP_FILE="${DIST_DIR}/${DIST_NAME}.zip"

echo "========================================================"
echo "   SoEasy 桌面搜索引擎一键打包系统"
echo "   版本: ${VERSION}  |  平台: ${PLATFORM}"
echo "========================================================"

# 1. 编译 Release 版本二进制
echo "[1/6] 编译最新 Release 版本二进制..."
cmake -B "${BUILD_DIR}" -S "${PROJECT_ROOT}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target soeasy-gui soeasy-worker -j4

# 2. 准备打包目录结构
echo "[2/6] 清理并构建分发目录结构..."
rm -rf "${STAGE_DIR}" "${ZIP_FILE}"
mkdir -p "${STAGE_DIR}/bin"
mkdir -p "${STAGE_DIR}/lib"
mkdir -p "${STAGE_DIR}/plugins/platforms"
mkdir -p "${STAGE_DIR}/dict"

# 3. 收集主可执行程序与工作沙箱程序
echo "[3/6] 收集可执行程序..."
cp -p "${BUILD_DIR}/soeasy-gui" "${STAGE_DIR}/bin/"
cp -p "${BUILD_DIR}/soeasy-worker" "${STAGE_DIR}/bin/"
chmod +x "${STAGE_DIR}/bin/"*

# 4. 收集核心运行时依赖库
echo "[4/6] 收集动态链接库与 Qt 平台插件..."
SYSROOT_LIB="${PROJECT_ROOT}/third_party/sysroot/usr/lib/x86_64-linux-gnu"
cp -d "${SYSROOT_LIB}"/libcapnp* "${STAGE_DIR}/lib/" 2>/dev/null || true
cp -d "${SYSROOT_LIB}"/libkj* "${STAGE_DIR}/lib/" 2>/dev/null || true
cp -d "${SYSROOT_LIB}"/libuchardet* "${STAGE_DIR}/lib/" 2>/dev/null || true
cp -d "${SYSROOT_LIB}"/libdoctotext* "${STAGE_DIR}/lib/" 2>/dev/null || true
cp -d "${SYSROOT_LIB}"/libwv2* "${STAGE_DIR}/lib/" 2>/dev/null || true
cp -d "${SYSROOT_LIB}"/libhtmlcxx* "${STAGE_DIR}/lib/" 2>/dev/null || true
cp -d "${SYSROOT_LIB}"/libmimetic* "${STAGE_DIR}/lib/" 2>/dev/null || true
cp -d "${SYSROOT_LIB}"/libcharsetdetect* "${STAGE_DIR}/lib/" 2>/dev/null || true
cp -d "${SYSROOT_LIB}"/libminizip* "${STAGE_DIR}/lib/" 2>/dev/null || true

# 收集 Qt5 xcb 平台插件
QT_XCB="/usr/lib/x86_64-linux-gnu/qt5/plugins/platforms/libqxcb.so"
if [ -f "${QT_XCB}" ]; then
  cp -p "${QT_XCB}" "${STAGE_DIR}/plugins/platforms/"
fi

# 5. 收集词典与资源文件
echo "[5/6] 收集结巴词典与启动引导脚本..."
cp -rp "${PROJECT_ROOT}/third_party/cppjieba/dict/"* "${STAGE_DIR}/dict/"
cp -p "${PROJECT_ROOT}/README.md" "${STAGE_DIR}/"

# 创建启动引导脚本 run.sh
cat << 'EOF' > "${STAGE_DIR}/run.sh"
#!/usr/bin/env bash
# SoEasy 独立启动引导脚本
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export LD_LIBRARY_PATH="${APP_DIR}/lib:${APP_DIR}/bin:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="${APP_DIR}/plugins:${QT_PLUGIN_PATH:-}"
export QT_QPA_PLATFORM_PLUGIN_PATH="${APP_DIR}/plugins/platforms:${QT_QPA_PLATFORM_PLUGIN_PATH:-}"

exec "${APP_DIR}/bin/soeasy-gui" --dict "${APP_DIR}/dict" --worker "${APP_DIR}/bin/soeasy-worker" "$@"
EOF
chmod +x "${STAGE_DIR}/run.sh"

# 创建命令行版本快速查询引导脚本 soeasy-cli.sh (可选便捷调用)
cat << 'EOF' > "${STAGE_DIR}/soeasy-worker.sh"
#!/usr/bin/env bash
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${APP_DIR}/lib:${LD_LIBRARY_PATH:-}"
exec "${APP_DIR}/bin/soeasy-worker" "$@"
EOF
chmod +x "${STAGE_DIR}/soeasy-worker.sh"

# 6. 打包生成独立 zip 归档
echo "[6/6] 生成独立 ZIP 压缩分发包..."
cd "${DIST_DIR}"
zip -r -q "${ZIP_FILE}" "${DIST_NAME}"
cd "${PROJECT_ROOT}"

ZIP_SIZE=$(du -h "${ZIP_FILE}" | cut -f1)
SHA256=$(sha256sum "${ZIP_FILE}" | cut -d' ' -f1)

echo "========================================================"
echo "🎉 打包完成!"
echo "📦 输出文件: ${ZIP_FILE}"
echo "📊 文件大小: ${ZIP_SIZE}"
echo "🔒 SHA256:  ${SHA256}"
echo "========================================================"
