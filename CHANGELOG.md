# Changelog

All notable changes to SoEasy are documented in this file.

## [v0.4.6] - 2026-11-26

### Packaging & Installers
- **Linux `.deb` Package (`ubuntu-24.04` / Debian amd64)**: Added `scripts/package_deb.sh` installing into `/opt/soeasy` with desktop integration (`/usr/share/applications/soeasy.desktop`, `/usr/bin/soeasy-gui`, `/usr/bin/soeasy-cli`) and `dpkg-shlibdeps` `split_soname()`-aligned `shlibs.local` rules to prevent private library leakage.
- **Windows x64 NSIS Installer & Portable Zip**: Added `packaging/windows/soeasy.nsi` (Windows 10+ 64-bit enforced, bilingual English / Simplified Chinese prompts, automatic running-instance check, optional per-user data cleanup on uninstall) and flattened staging layout via `scripts/package_win64_stage.sh`.
- **CI End-to-End Smoke Verification**: Added clean-container `ubuntu:24.04` `.deb` install/CLI/Xvfb GUI/purge smoke tests and headless Windows NSIS silent install/CLI/GUI/uninstall smoke tests in `.github/workflows/build.yml`.

### Build & Dependencies
- **Minimal Static OpenCV (`third-party-v1`)**: Added `.github/workflows/build-opencv.yml` to build minimal static OpenCV `4.10.0` (`core`, `imgproc`, `imgcodecs`, bundled JPEG/PNG/TIFF/WebP) for Linux x86_64 and Windows x64, eliminating heavyweight dynamic dependencies (`gdal`, `poppler`, `vtk`, etc.) from release packages while retaining automatic system OpenCV fallback.
- **Unified Version Tracking**: Set `project(soeasy VERSION 0.4.6)` in `CMakeLists.txt`, wired `SOEASY_VERSION` into `soeasy-gui` and `soeasy-cli --version`, and scoped `git describe --match 'v*'` across packaging scripts and CI workflows.

### CLI & OCR Refinements
- **Cross-Platform AppData Alignment**: Aligned `soeasy-cli` default database/config directory resolution with `soeasy-gui` (`~/.local/share/SoEasy` on Linux, `%APPDATA%\SoEasy` on Windows, with legacy path fallback).
- **Unified Supported Image Extensions**: Consolidated supported image OCR extensions in `src/common/format_categories.h` (`png`, `jpg`, `jpeg`, `jfif`, `bmp`, `webp`, `tiff`, `tif`, `svg`, `pnm`, `ppm`, `pgm`), shared across worker parsing, path filtering, timeout selection, GUI icons, and plain-text preview guards; excluded unsupported `gif`/`tga` and logged multi-frame GIF OCR (`stb_image`) as `FEAT-012` in `TODOLIST.md`.

## [v0.4.5] - 2026-11-25

- Normalized Windows `doc_id` and `failed_files` paths across index orchestration and file crawler.
- Hardened Windows CTest teardown across indexing, logging, and GUI smoke tests.
