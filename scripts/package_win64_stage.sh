#!/usr/bin/env bash
# Copyright 2026 SoEasy Team. All rights reserved.
# Shared Windows x64 staging script used by both GitHub Actions and Docker cross-builds.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR=""
STAGE_DIR=""
MINGW_BIN=""
QT_DIR=""
WINDEPLOYQT_BIN=""
BUILD_DAT_DICT_BIN=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            [[ $# -lt 2 ]] && { echo "Error: --build-dir requires an argument" >&2; exit 2; }
            BUILD_DIR="$2"
            shift 2
            ;;
        --stage-dir)
            [[ $# -lt 2 ]] && { echo "Error: --stage-dir requires an argument" >&2; exit 2; }
            STAGE_DIR="$2"
            shift 2
            ;;
        --mingw-bin)
            [[ $# -lt 2 ]] && { echo "Error: --mingw-bin requires an argument" >&2; exit 2; }
            MINGW_BIN="$2"
            shift 2
            ;;
        --qt-dir)
            [[ $# -lt 2 ]] && { echo "Error: --qt-dir requires an argument" >&2; exit 2; }
            QT_DIR="$2"
            shift 2
            ;;
        --windeployqt)
            [[ $# -lt 2 ]] && { echo "Error: --windeployqt requires an argument" >&2; exit 2; }
            WINDEPLOYQT_BIN="$2"
            shift 2
            ;;
        --build-dat-dict)
            [[ $# -lt 2 ]] && { echo "Error: --build-dat-dict requires an argument" >&2; exit 2; }
            BUILD_DAT_DICT_BIN="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ -z "${BUILD_DIR}" || -z "${STAGE_DIR}" || -z "${MINGW_BIN}" || -z "${QT_DIR}" ]]; then
    echo "Usage: $0 --build-dir <dir> --stage-dir <dir> --mingw-bin <dir> --qt-dir <dir> [--windeployqt <exe>] [--build-dat-dict <exe>]" >&2
    exit 2
fi

mkdir -p \
    "${STAGE_DIR}/platforms" \
    "${STAGE_DIR}/imageformats" \
    "${STAGE_DIR}/styles" \
    "${STAGE_DIR}/dict" \
    "${STAGE_DIR}/fonts" \
    "${STAGE_DIR}/models" \
    "${STAGE_DIR}/icons"

# 1. Copy compiled Windows binaries directly to root (standard flat Windows layout)
cp -p "${BUILD_DIR}/soeasy-gui.exe" "${STAGE_DIR}/"
cp -p "${BUILD_DIR}/soeasy-worker.exe" "${STAGE_DIR}/"
if [[ -f "${BUILD_DIR}/soeasy-cli.exe" ]]; then
    cp -p "${BUILD_DIR}/soeasy-cli.exe" "${STAGE_DIR}/"
fi

# 2. Run windeployqt if available (native Windows runner)
if [[ -n "${WINDEPLOYQT_BIN}" && -x "${WINDEPLOYQT_BIN}" ]]; then
    "${WINDEPLOYQT_BIN}" --release --no-translations --no-opengl-sw "${STAGE_DIR}/soeasy-gui.exe" || true
fi

# 3. Ensure jieba_dat.bin exists and copy runtime dictionaries, models, and icons
if [[ ! -f "${PROJECT_ROOT}/third_party/cppjieba/dict/jieba_dat.bin" ]] && [[ -n "${BUILD_DAT_DICT_BIN}" && -x "${BUILD_DAT_DICT_BIN}" ]]; then
    "${BUILD_DAT_DICT_BIN}" "${PROJECT_ROOT}/third_party/cppjieba/dict/jieba.dict.utf8" "${PROJECT_ROOT}/third_party/cppjieba/dict/jieba_dat.bin"
fi
cp -p "${PROJECT_ROOT}/third_party/cppjieba/dict/jieba_dat.bin" "${STAGE_DIR}/dict/"
cp -p "${PROJECT_ROOT}/third_party/cppjieba/dict/hmm_model.utf8" "${STAGE_DIR}/dict/"
cp -p "${PROJECT_ROOT}/third_party/cppjieba/dict/user.dict.utf8" "${STAGE_DIR}/dict/" 2>/dev/null || touch "${STAGE_DIR}/dict/user.dict.utf8"
cp -p "${PROJECT_ROOT}/third_party/hyphen/hyph_en_US.dic" "${STAGE_DIR}/dict/"
cp -p "${PROJECT_ROOT}/resources/fonts/"*.otf "${STAGE_DIR}/fonts/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/resources/fonts/"*.ttf "${STAGE_DIR}/fonts/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/resources/fonts/"*.txt "${STAGE_DIR}/fonts/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/resources/fonts/"*.md "${STAGE_DIR}/fonts/" 2>/dev/null || true
cp -rp "${PROJECT_ROOT}/models/ppocr_v6" "${STAGE_DIR}/models/" 2>/dev/null || true
cp -rp "${PROJECT_ROOT}/resources/icons/"* "${STAGE_DIR}/icons/" 2>/dev/null || true

# 4. Copy third-party DLLs directly to root (preferring MinGW sysroot runtime over legacy doctotext copies)
cp -p "${PROJECT_ROOT}/third_party/doctotext/lib/doctotext.dll" "${STAGE_DIR}/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/third_party/doctotext/lib/pthreadGC2.dll" "${STAGE_DIR}/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/third_party/doctotext/lib/libiconv"*.dll "${STAGE_DIR}/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/third_party/doctotext/lib/libwv2"*.dll "${STAGE_DIR}/" 2>/dev/null || true
cp -p "${PROJECT_ROOT}/third_party/onnxruntime/lib/"*.dll "${STAGE_DIR}/" 2>/dev/null || true

# 5. Copy Qt plugins explicitly beside the executable
QT_PLUGIN_ROOT="${QT_DIR}/plugins"
if [[ ! -d "${QT_PLUGIN_ROOT}" && -d "${QT_DIR}/lib/qt5/plugins" ]]; then
    QT_PLUGIN_ROOT="${QT_DIR}/lib/qt5/plugins"
fi
cp -p "${QT_PLUGIN_ROOT}/platforms/qwindows.dll" "${STAGE_DIR}/platforms/" 2>/dev/null || true
cp -p "${QT_PLUGIN_ROOT}/styles/"*.dll "${STAGE_DIR}/styles/" 2>/dev/null || true
cp -p "${QT_PLUGIN_ROOT}/imageformats/qico.dll" "${STAGE_DIR}/imageformats/" 2>/dev/null || true
cp -p "${QT_PLUGIN_ROOT}/imageformats/qjpeg.dll" "${STAGE_DIR}/imageformats/" 2>/dev/null || true

# 6. Ensure modern MSYS2/MinGW C++ runtime overrides any older copies
cp -p "${MINGW_BIN}/libgcc"*.dll "${STAGE_DIR}/" 2>/dev/null || true
cp -p "${MINGW_BIN}/libstdc"*.dll "${STAGE_DIR}/" 2>/dev/null || true
cp -p "${MINGW_BIN}/libwinpthread"*.dll "${STAGE_DIR}/" 2>/dev/null || true

# 7. Automated recursive PE dependency convergence
SEARCH_DIRS=("${MINGW_BIN}")
if [[ -d "${QT_DIR}/bin" ]]; then
    SEARCH_DIRS+=("${QT_DIR}/bin")
fi
SEARCH_DIRS+=("${PROJECT_ROOT}/third_party/doctotext/lib" "${PROJECT_ROOT}/third_party/onnxruntime/lib")

export PYTHONIOENCODING=utf-8
python3 "${PROJECT_ROOT}/scripts/collect_win64_deps.py" \
    --target-dir "${STAGE_DIR}" \
    --search-dirs "${SEARCH_DIRS[@]}"
