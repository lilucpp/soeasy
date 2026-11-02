#!/usr/bin/env python3
# Copyright 2026 SoEasy Team. All rights reserved.
# Automated PE dependency resolver for MinGW Win64 cross-compiled builds.

import argparse
import os
import shutil
import subprocess
import sys

# System DLLs provided natively by Windows OS (Windows 7/10/11)
SYSTEM_DLL_PREFIXES = (
    "api-ms-win-",
    "ext-ms-win-",
)

SYSTEM_DLLS = {
    "advapi32.dll",
    "bcrypt.dll",
    "cfgmgr32.dll",
    "comctl32.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "d2d1.dll",
    "d3d11.dll",
    "dnsapi.dll",
    "dwrite.dll",
    "dwmapi.dll",
    "dxgi.dll",
    "gdi32.dll",
    "glu32.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "mpr.dll",
    "msvcrt.dll",
    "ncrypt.dll",
    "netapi32.dll",
    "normaliz.dll",
    "ntdll.dll",
    "odbc32.dll",
    "odbccp32.dll",
    "ole32.dll",
    "oleaut32.dll",
    "opengl32.dll",
    "psapi.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "setupapi.dll",
    "shell32.dll",
    "shlwapi.dll",
    "sspicli.dll",
    "user32.dll",
    "userenv.dll",
    "uxtheme.dll",
    "version.dll",
    "winmm.dll",
    "winspool.drv",
    "wldap32.dll",
    "ws2_32.dll",
    "wsock32.dll",
    "wtsapi32.dll",
    "uuid.dll",
}

def is_system_dll(name: str) -> bool:
    low = name.lower()
    if low in SYSTEM_DLLS:
        return True
    return any(low.startswith(p) for p in SYSTEM_DLL_PREFIXES)

def find_objdump(custom_path: str = None) -> str:
    candidates = []
    if custom_path:
        candidates.append(custom_path)
    candidates.extend([
        "x86_64-w64-mingw32-objdump",
        "objdump",
        "llvm-objdump"
    ])
    for cand in candidates:
        if shutil.which(cand):
            return cand
    return custom_path or "x86_64-w64-mingw32-objdump"

def get_pe_imports(filepath: str, objdump_bin: str) -> list:
    try:
        out = subprocess.check_output(
            [objdump_bin, "-p", filepath],
            text=True,
            stderr=subprocess.DEVNULL,
        )
    except Exception as e:
        print(f"Warning: Failed to dump PE headers for {filepath}: {e}", file=sys.stderr)
        return []

    imports = []
    for line in out.splitlines():
        if "DLL Name:" in line:
            dll = line.split("DLL Name:")[1].strip()
            imports.append(dll)
    return imports

def main():
    parser = argparse.ArgumentParser(description="Collect PE runtime dependencies for MinGW win64.")
    parser.add_argument("--target-dir", required=True, help="Directory containing exe/dll files (e.g. dist/.../bin)")
    parser.add_argument("--search-dirs", nargs="+", required=True, help="Directories to search for dependencies")
    parser.add_argument("--objdump", default=None, help="Path to objdump binary (auto-detected by default)")
    args = parser.parse_args()

    objdump_cmd = find_objdump(args.objdump)
    target_dir = os.path.abspath(args.target_dir)
    search_dirs = [os.path.abspath(d) for d in args.search_dirs if os.path.isdir(d)]

    if not os.path.isdir(target_dir):
        print(f"Error: Target directory {target_dir} does not exist.", file=sys.stderr)
        sys.exit(1)

    print("========================================================")
    print("   SoEasy PE 依赖库自动递归收集工具 (MinGW Win64)")
    print(f"   目标目录: {target_dir}")
    print(f"   搜索路径: {search_dirs}")
    print("========================================================")

    # Cache directory file lists for fast case-insensitive lookup
    dir_cache = {}
    for sdir in search_dirs:
        dir_cache[sdir] = {}
        for entry in os.listdir(sdir):
            dir_cache[sdir][entry.lower()] = os.path.join(sdir, entry)

    collected_lower = set()
    for root, _, files in os.walk(target_dir):
        for f in files:
            if f.lower().endswith(".dll"):
                collected_lower.add(f.lower())

    round_idx = 1
    unresolved = set()

    while True:
        new_copies = []
        all_binaries = []
        for root, _, files in os.walk(target_dir):
            for f in files:
                if f.lower().endswith((".exe", ".dll")):
                    all_binaries.append(os.path.join(root, f))

        for binary in all_binaries:
            for imp in get_pe_imports(binary, objdump_cmd):
                if is_system_dll(imp):
                    continue
                imp_lower = imp.lower()
                if imp_lower in collected_lower:
                    continue

                # Find candidate in search directories
                candidate = None
                for sdir in search_dirs:
                    if imp_lower in dir_cache[sdir]:
                        candidate = dir_cache[sdir][imp_lower]
                        break

                if candidate:
                    dst = os.path.join(target_dir, os.path.basename(candidate))
                    shutil.copy2(candidate, dst)
                    collected_lower.add(imp_lower)
                    new_copies.append((imp, candidate, dst))
                else:
                    unresolved.add(imp)

        if not new_copies:
            break

        print(f"[第 {round_idx} 轮] 成功收集并补充 {len(new_copies)} 个动态库:")
        for imp, src, dst in new_copies:
            print(f"  + {imp} <- {src}")
        round_idx += 1

    print("========================================================")
    print(f"依赖收集完成! 目标目录当前共有 {len(collected_lower)} 个动态链接库。")
    if unresolved:
        print("注意: 以下非系统库未在搜索路径中找到 (可能由 MSVC 运行库或外部提供):")
        for u in sorted(unresolved):
            print(f"  - {u}")
    print("========================================================")

if __name__ == "__main__":
    main()
