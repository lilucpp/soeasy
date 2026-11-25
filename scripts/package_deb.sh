#!/usr/bin/env bash
# Copyright 2026 SoEasy Team. All rights reserved.
# Build a Debian/Ubuntu (.deb) installer package for SoEasy Desktop Search Engine.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

VERSION=""
STAGE_DIR=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stage-dir)
            if [[ $# -lt 2 ]]; then
                echo "Error: --stage-dir requires a directory argument" >&2
                exit 2
            fi
            STAGE_DIR="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [VERSION] [--stage-dir <path>]"
            exit 0
            ;;
        *)
            if [[ -z "${VERSION}" ]]; then
                VERSION="$1"
            else
                echo "Unknown argument: $1" >&2
                exit 2
            fi
            shift
            ;;
    esac
done

RAW_TAG="$(git -C "${PROJECT_ROOT}" describe --tags --abbrev=0 2>/dev/null || true)"
VERSION="${VERSION:-${RAW_TAG#v}}"
VERSION="${VERSION:-0.4.5}"
# Debian package versions must start with a digit and should not contain spaces
DEB_VERSION="$(echo "${VERSION}" | sed 's/^[^0-9]*//; s/-/~/g')"
if [[ -z "${DEB_VERSION}" ]]; then
    DEB_VERSION="0.4.5"
fi

ARCH="$(dpkg --print-architecture 2>/dev/null || echo "amd64")"
DIST_DIR="${PROJECT_ROOT}/dist"
USER_STAGE_DIR="${STAGE_DIR}"
STAGE_DIR="${STAGE_DIR:-${DIST_DIR}/soeasy-v${VERSION}-linux-x86_64}"
DEB_ROOT="${DIST_DIR}/deb_root_${DEB_VERSION}_${ARCH}"
DEB_FILE="${DIST_DIR}/soeasy_${DEB_VERSION}_${ARCH}.deb"

echo "========================================================"
echo "   SoEasy Debian/Ubuntu (.deb) 打包系统"
echo "   版本: ${DEB_VERSION}  |  架构: ${ARCH}"
echo "========================================================"

if [[ ! -d "${STAGE_DIR}/bin" ]]; then
    if [[ -n "${USER_STAGE_DIR}" ]]; then
        echo "Error: 指定分发目录不存在或不完整: ${STAGE_DIR}" >&2
        exit 1
    fi
    echo "[1/5] 未检测到现成分发目录 ${STAGE_DIR}，先执行 scripts/package.sh..."
    bash "${SCRIPT_DIR}/package.sh" "${VERSION}"
else
    echo "[1/5] 复用已构建分发目录: ${STAGE_DIR}"
fi

echo "[2/5] 构建 FHS 规范目录树 (/opt/soeasy + /usr/bin + /usr/share)..."
rm -rf "${DEB_ROOT}" "${DEB_FILE}"
mkdir -p "${DEB_ROOT}/DEBIAN"
mkdir -p "${DEB_ROOT}/opt/soeasy"
mkdir -p "${DEB_ROOT}/usr/bin"
mkdir -p "${DEB_ROOT}/usr/share/soeasy"
mkdir -p "${DEB_ROOT}/usr/share/applications"
mkdir -p "${DEB_ROOT}/usr/share/icons/hicolor/256x256/apps"
mkdir -p "${DEB_ROOT}/etc/sysctl.d"

# 拷贝核心运行目录到 /opt/soeasy (仅保留运行时文件，剔除便携包专用脚本及静态库)
cp -a "${STAGE_DIR}/bin" "${DEB_ROOT}/opt/soeasy/"
cp -a "${STAGE_DIR}/lib" "${DEB_ROOT}/opt/soeasy/"
cp -a "${STAGE_DIR}/plugins" "${DEB_ROOT}/opt/soeasy/"
cp -a "${STAGE_DIR}/dict" "${DEB_ROOT}/opt/soeasy/"
cp -a "${STAGE_DIR}/models" "${DEB_ROOT}/opt/soeasy/"
cp -a "${STAGE_DIR}/icons" "${DEB_ROOT}/opt/soeasy/"
cp -p "${STAGE_DIR}/run.sh" "${DEB_ROOT}/opt/soeasy/"
if [[ -f "${STAGE_DIR}/README.md" ]]; then
    cp -p "${STAGE_DIR}/README.md" "${DEB_ROOT}/opt/soeasy/"
fi
find "${DEB_ROOT}/opt/soeasy/lib" -maxdepth 1 -name '*.a' -delete 2>/dev/null || true
chmod 0755 "${DEB_ROOT}/opt/soeasy/bin/"* "${DEB_ROOT}/opt/soeasy/run.sh"

# 创建 /usr/bin 全局入口脚本
cat << 'EOF' > "${DEB_ROOT}/usr/bin/soeasy-gui"
#!/usr/bin/env bash
exec /opt/soeasy/run.sh "$@"
EOF
chmod 0755 "${DEB_ROOT}/usr/bin/soeasy-gui"

cat << 'EOF' > "${DEB_ROOT}/usr/bin/soeasy-cli"
#!/usr/bin/env bash
export LD_LIBRARY_PATH="/opt/soeasy/lib:${LD_LIBRARY_PATH:-}"
exec /opt/soeasy/bin/soeasy-cli "$@"
EOF
chmod 0755 "${DEB_ROOT}/usr/bin/soeasy-cli"

# 软链接词典到标准共享路径，确保 soeasy-cli 在任意工作目录下无需 --dict 即可自动发现
ln -s /opt/soeasy/dict "${DEB_ROOT}/usr/share/soeasy/dict"

# 安装标准桌面菜单项与主题图标
if [[ -f "${PROJECT_ROOT}/resources/icons/soeasy.png" ]]; then
    cp -p "${PROJECT_ROOT}/resources/icons/soeasy.png" "${DEB_ROOT}/usr/share/icons/hicolor/256x256/apps/soeasy.png"
    chmod 0644 "${DEB_ROOT}/usr/share/icons/hicolor/256x256/apps/soeasy.png"
fi

cat << 'EOF' > "${DEB_ROOT}/usr/share/applications/soeasy.desktop"
[Desktop Entry]
Version=1.0
Type=Application
Name=SoEasy
GenericName=Desktop Full-Text Search
Comment=SoEasy Desktop Search Engine - 让电脑没有难搜的文件
Exec=/usr/bin/soeasy-gui %F
Icon=soeasy
Terminal=false
Categories=Utility;
Keywords=search;file;fts;ocr;desktop;
StartupNotify=true
StartupWMClass=soeasy-gui
EOF
chmod 0644 "${DEB_ROOT}/usr/share/applications/soeasy.desktop"

# 安装期调优 inotify 监听配额 (60- 前缀保留宿主机管理员 99- 配置的最高优先级)
cat << 'EOF' > "${DEB_ROOT}/etc/sysctl.d/60-soeasy-inotify.conf"
# Installed by SoEasy so real-time file watching works across large directories as a normal user
fs.inotify.max_user_watches=524288
EOF
chmod 0644 "${DEB_ROOT}/etc/sysctl.d/60-soeasy-inotify.conf"

echo "[3/5] 计算系统动态库依赖 (dpkg-shlibdeps)..."
DEPENDS_LINE="libc6, libstdc++6, libqt5widgets5, libqt5gui5, libqt5core5a, libqt5network5"
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    SHLIB_TMP="$(mktemp -d)"
    mkdir -p "${SHLIB_TMP}/debian"
    cat << 'EOF' > "${SHLIB_TMP}/debian/control"
Source: soeasy
Section: utils
Priority: optional
Maintainer: SoEasy Team <dev@soeasy.local>

Package: soeasy
Architecture: any
EOF
    # 为随包私有的 /opt/soeasy/lib 动态库生成本地 shlibs 映射，避免 dpkg-shlibdeps 报错缺少系统包归属
    : > "${SHLIB_TMP}/debian/shlibs.local"
    for so_file in "${DEB_ROOT}/opt/soeasy/lib/"*.so*; do
        [[ -e "${so_file}" ]] || continue
        soname="$(objdump -p "${so_file}" 2>/dev/null | awk '/SONAME/{print $2; exit}' || true)"
        if [[ -z "${soname}" ]]; then
            soname="$(basename "${so_file}")"
        fi
        lib_base="${soname%.so*}"
        so_ver="${soname#*.so.}"
        if [[ "${so_ver}" == "${soname}" ]]; then
            so_ver="0"
        fi
        echo "${lib_base} ${so_ver}" >> "${SHLIB_TMP}/debian/shlibs.local"
    done
    sort -u -o "${SHLIB_TMP}/debian/shlibs.local" "${SHLIB_TMP}/debian/shlibs.local"

    SHLIB_TARGETS=()
    for f in "${DEB_ROOT}/opt/soeasy/bin/"* \
             "${DEB_ROOT}/opt/soeasy/lib/"*.so* \
             "${DEB_ROOT}/opt/soeasy/plugins/platforms/"*.so \
             "${DEB_ROOT}/opt/soeasy/plugins/imageformats/"*.so; do
        if [[ -f "${f}" && ! -L "${f}" ]]; then
            SHLIB_TARGETS+=("${f}")
        fi
    done

    if (cd "${SHLIB_TMP}" && dpkg-shlibdeps \
            -l"${DEB_ROOT}/opt/soeasy/lib" \
            --ignore-missing-info \
            -O "${SHLIB_TARGETS[@]}" > "${SHLIB_TMP}/substvars"); then
        COMPUTED_DEPS="$(grep -m1 '^shlibs:Depends=' "${SHLIB_TMP}/substvars" | cut -d= -f2- || true)"
        if [[ -n "${COMPUTED_DEPS}" ]]; then
            DEPENDS_LINE="${COMPUTED_DEPS}"
        fi
    else
        echo "[Warn] dpkg-shlibdeps 执行失败，回退到基础依赖列表" >&2
    fi
    rm -rf "${SHLIB_TMP}"
fi
echo "  -> Depends: ${DEPENDS_LINE}"

echo "[4/5] 生成 DEBIAN 元数据与安装钩子 (postinst / postrm)..."
INSTALLED_SIZE="$(du -sk "${DEB_ROOT}/opt" "${DEB_ROOT}/usr" "${DEB_ROOT}/etc" | awk '{sum+=$1} END {print sum}')"

cat << EOF > "${DEB_ROOT}/DEBIAN/control"
Package: soeasy
Version: ${DEB_VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Maintainer: SoEasy Team <dev@soeasy.local>
Homepage: https://github.com/lilucpp/soeasy
Depends: ${DEPENDS_LINE}
Recommends: desktop-file-utils, shared-mime-info
Description: Native cross-platform desktop full-text search engine
 SoEasy is a fast, local-first desktop full-text search engine built with
 modern C++17 and Qt5. It indexes source code, Office/PDF documents, and
 scanned images (PP-OCRv6) into an encrypted SQLite FTS5 store, providing
 both a native desktop UI (soeasy-gui) and a headless CLI (soeasy-cli).
EOF

cat << 'EOF' > "${DEB_ROOT}/DEBIAN/conffiles"
/etc/sysctl.d/60-soeasy-inotify.conf
EOF

cat << 'EOF' > "${DEB_ROOT}/DEBIAN/postinst"
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
    if [ -w /proc/sys/fs/inotify/max_user_watches ]; then
        CUR_WATCHES="$(cat /proc/sys/fs/inotify/max_user_watches 2>/dev/null || echo 0)"
        if [ "${CUR_WATCHES:-0}" -lt 524288 ] 2>/dev/null && command -v sysctl >/dev/null 2>&1; then
            sysctl -p /etc/sysctl.d/60-soeasy-inotify.conf >/dev/null 2>&1 || true
        fi
    fi
fi
exit 0
EOF

cat << 'EOF' > "${DEB_ROOT}/DEBIAN/postrm"
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi
if [ "$1" = "purge" ]; then
    echo "SoEasy system files purged. Per-user index data (if any) is preserved under ~/.local/share/SoEasy/."
fi
exit 0
EOF

chmod 0755 "${DEB_ROOT}/DEBIAN/postinst" "${DEB_ROOT}/DEBIAN/postrm"
chmod 0644 "${DEB_ROOT}/DEBIAN/control" "${DEB_ROOT}/DEBIAN/conffiles"

echo "[5/5] 打包生成 .deb 安装包..."
dpkg-deb --root-owner-group --build "${DEB_ROOT}" "${DEB_FILE}"
rm -rf "${DEB_ROOT}"

DEB_SIZE="$(du -h "${DEB_FILE}" | cut -f1)"
SHA256="$(sha256sum "${DEB_FILE}" | cut -d' ' -f1)"

echo "========================================================"
echo "🎉 Debian (.deb) 打包完成!"
echo "📦 输出文件: ${DEB_FILE}"
echo "📊 文件大小: ${DEB_SIZE}"
echo "🔒 SHA256:  ${SHA256}"
echo "💡 安装测试: sudo apt install ${DEB_FILE}"
echo "========================================================"
