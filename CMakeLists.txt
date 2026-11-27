cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()
project(soeasy VERSION 0.4.6 LANGUAGES C CXX)

add_compile_definitions(
    SOEASY_PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    SOEASY_VERSION="${PROJECT_VERSION}"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 5.15 COMPONENTS Core Gui Widgets Network REQUIRED)
if(NOT DEFINED OpenCV_DIR OR OpenCV_DIR STREQUAL "")
    if(WIN32)
        set(_SOEASY_OPENCV_PREBUILT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv/windows-x64")
    else()
        set(_SOEASY_OPENCV_PREBUILT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv/linux-x86_64")
    endif()
    foreach(_OCV_CANDIDATE
        "${_SOEASY_OPENCV_PREBUILT_ROOT}/lib/cmake/opencv4"
        "${_SOEASY_OPENCV_PREBUILT_ROOT}/x64/mingw/staticlib"
        "${_SOEASY_OPENCV_PREBUILT_ROOT}/x64/mingw/lib"
        "${_SOEASY_OPENCV_PREBUILT_ROOT}"
    )
        if(EXISTS "${_OCV_CANDIDATE}/OpenCVConfig.cmake")
            set(OpenCV_DIR "${_OCV_CANDIDATE}")
            if(WIN32 AND NOT DEFINED OpenCV_STATIC)
                set(OpenCV_STATIC ON)
            endif()
            message(STATUS "[OpenCV] Using prebuilt minimal static OpenCV from: ${OpenCV_DIR}")
            break()
        endif()
    endforeach()
endif()
find_package(OpenCV REQUIRED)
set(Boost_USE_STATIC_LIBS OFF)
find_package(Boost 1.73 REQUIRED COMPONENTS nowide)

# Optimization flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0 -Wall)
else()
    add_compile_options(-O3 -Wall)
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _Frees_ptr_opt_=)
endif()

option(BUILD_TESTS "Build test suite" ON)
option(ENABLE_SANITY "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(ENABLE_SANITY)
    message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer (-fsanitize=address,undefined)")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Configure portable transitive RPATH (using DT_RPATH instead of DT_RUNPATH for transitive dependency lookup)
if(UNIX AND NOT APPLE)
    add_link_options("-Wl,--disable-new-dtags")
    set(CMAKE_SKIP_BUILD_RPATH FALSE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    set(CMAKE_BUILD_RPATH
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctotext/lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sysroot/usr/lib/x86_64-linux-gnu"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib"
        "\$ORIGIN/../lib"
        "\$ORIGIN/lib"
    )
    set(CMAKE_INSTALL_RPATH "\$ORIGIN/../lib;\$ORIGIN/lib")
endif()

# DocToText prebuilt binary dependency
include(third_party/doctotext/doctotext.cmake)

# ONNX Runtime Windows dependency setup (local win-env -> win-env release -> NuGet -> GitHub fallback)
set(_ORT_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib")
if(WIN32 AND NOT EXISTS "${_ORT_LIB_DIR}/onnxruntime.lib")
    if(EXISTS "D:/msys64/opt/onnxruntime/lib/onnxruntime.lib")
        message(STATUS "[ONNXRuntime] Copying prebuilt Windows x64 binaries from D:/msys64/opt/onnxruntime/lib...")
        file(GLOB _ORT_LOCAL_LIBS
            "D:/msys64/opt/onnxruntime/lib/*.lib"
            "D:/msys64/opt/onnxruntime/lib/*.dll"
        )
        file(COPY ${_ORT_LOCAL_LIBS} DESTINATION "${_ORT_LIB_DIR}/")
    else()
        set(_ORT_MIRRORS
            "https://github.com/lilucpp/soeasy/releases/download/win-env-v1/onnxruntime-win-x64-1.20.0.zip"
            "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.20.0/microsoft.ml.onnxruntime.1.20.0.nupkg"
            "https://github.com/microsoft/onnxruntime/releases/download/v1.20.0/onnxruntime-win-x64-1.20.0.zip"
        )
        foreach(_ORT_URL ${_ORT_MIRRORS})
            if(EXISTS "${_ORT_LIB_DIR}/onnxruntime.lib")
                break()
            endif()
            message(STATUS "[ONNXRuntime] Downloading Windows x64 v1.20.0 from: ${_ORT_URL}")
            set(_ORT_ZIP "${CMAKE_BINARY_DIR}/onnxruntime-win-x64-1.20.0.zip")
            file(REMOVE "${_ORT_ZIP}")
            file(DOWNLOAD "${_ORT_URL}" "${_ORT_ZIP}" SHOW_PROGRESS STATUS _ORT_DL_STATUS TIMEOUT 120)
            list(GET _ORT_DL_STATUS 0 _ORT_DL_RET)
            if(_ORT_DL_RET EQUAL 0)
                set(_ORT_UNPACK "${CMAKE_BINARY_DIR}/ort_unpack")
                file(REMOVE_RECURSE "${_ORT_UNPACK}")
                file(MAKE_DIRECTORY "${_ORT_UNPACK}")
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E tar xvf "${_ORT_ZIP}"
                    WORKING_DIRECTORY "${_ORT_UNPACK}"
                    RESULT_VARIABLE _ORT_TAR_RET
                )
                if(_ORT_TAR_RET EQUAL 0)
                    file(GLOB_RECURSE _ORT_FOUND_LIBS
                        "${_ORT_UNPACK}/runtimes/win-x64/native/*.lib"
                        "${_ORT_UNPACK}/runtimes/win-x64/native/*.dll"
                    )
                    if(NOT _ORT_FOUND_LIBS)
                        file(GLOB_RECURSE _ORT_FOUND_LIBS
                            "${_ORT_UNPACK}/*.lib"
                            "${_ORT_UNPACK}/*.dll"
                        )
                    endif()
                    foreach(f ${_ORT_FOUND_LIBS})
                        file(COPY "${f}" DESTINATION "${_ORT_LIB_DIR}/")
                    endforeach()
                endif()
            else()
                list(GET _ORT_DL_STATUS 1 _ORT_DL_MSG)
                message(WARNING "[ONNXRuntime] Download failed from ${_ORT_URL}: ${_ORT_DL_MSG}")
            endif()
        endforeach()
    endif()

    if(NOT EXISTS "${_ORT_LIB_DIR}/onnxruntime.lib")
        message(FATAL_ERROR "[ONNXRuntime] Failed to install Windows x64 ONNX Runtime binaries from all mirrors.")
    endif()
    message(STATUS "[ONNXRuntime] Windows x64 binaries installed successfully.")
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Sysroot include and link paths
if(NOT WIN32)
    include_directories(SYSTEM
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sysroot/usr/include
    )
    link_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sysroot/usr/lib/x86_64-linux-gnu
    )
endif()
include_directories(SYSTEM
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctotext/include
)
link_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctotext/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib
)

# -------------------------------------------------------------
# 1. Third-party: Zstandard (zstd) static library
# -------------------------------------------------------------
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/zstd/build/cmake ${CMAKE_BINARY_DIR}/zstd_build EXCLUDE_FROM_ALL)

# -------------------------------------------------------------
# 2. Third-party: SQLite3MultipleCiphers (sqlite3mc)
# -------------------------------------------------------------
add_library(sqlite3mc STATIC
    third_party/sqlite3mc/sqlite3mc_amalgamation.c
)
target_include_directories(sqlite3mc PUBLIC
    third_party/sqlite3mc
)
target_compile_definitions(sqlite3mc PUBLIC
    SQLITE_ENABLE_FTS5=1
    SQLITE_ENABLE_JSON1=1
    SQLITE_ENABLE_RTREE=1
    SQLITE_ENABLE_DESERIALIZE=1
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_DBSTAT_VTAB=1
)
if(MSVC)
    target_compile_options(sqlite3mc PRIVATE /w /O2)
else()
    # SQLite official guideline: Use -O2 to avoid instruction cache bloat and massive compile-time penalty of -O3 on the 13MB amalgamation
    target_compile_options(sqlite3mc PRIVATE -w -O2)
endif()
if(WIN32)
    target_link_libraries(sqlite3mc PRIVATE pthread)
else()
    target_link_libraries(sqlite3mc PRIVATE dl pthread)
endif()

# -------------------------------------------------------------
# 2.1 Third-party: SQLiteCpp (Modern C++ SQLite3 wrapper)
# -------------------------------------------------------------
add_library(SQLiteCpp STATIC
    third_party/SQLiteCpp/src/Backup.cpp
    third_party/SQLiteCpp/src/Column.cpp
    third_party/SQLiteCpp/src/Database.cpp
    third_party/SQLiteCpp/src/Exception.cpp
    third_party/SQLiteCpp/src/Savepoint.cpp
    third_party/SQLiteCpp/src/Statement.cpp
    third_party/SQLiteCpp/src/Transaction.cpp
)
target_include_directories(SQLiteCpp PUBLIC
    third_party/SQLiteCpp/include
    third_party/sqlite3mc
)
target_link_libraries(SQLiteCpp PUBLIC sqlite3mc)

# -------------------------------------------------------------
# 3. Third-party: efsw (Entropia File System Watcher)
# -------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TEST_APP OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/efsw ${CMAKE_BINARY_DIR}/efsw_build EXCLUDE_FROM_ALL)

# -------------------------------------------------------------
# 3.1 Third-party: subprocess.h (Single-header Process Management)
# -------------------------------------------------------------
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/third_party/subprocess)

# -------------------------------------------------------------
# 4. Common Library (spdlog Logging + System Utilities)
# -------------------------------------------------------------
add_library(soeasy_common STATIC
    src/common/logger.cpp
    src/common/encoding_utils.cpp
    src/common/system_env.cpp
    src/common/snippet_highlighter.cpp
    src/common/machine_key_provider.cpp
)
target_include_directories(soeasy_common PUBLIC
    src
    third_party/spdlog/include
    third_party/utfcpp/source
)
if(WIN32)
    target_link_libraries(soeasy_common PUBLIC pthread Boost::nowide advapi32)
else()
    target_link_libraries(soeasy_common PUBLIC rt pthread Boost::nowide)
endif()

# -------------------------------------------------------------
# 5. Cap'n Proto Schema & RPC Library
# -------------------------------------------------------------
add_library(soeasy_schema STATIC
    schema/soeasy_ipc.capnp.c++
)
target_include_directories(soeasy_schema PUBLIC
    schema
)
if(NOT WIN32)
    target_include_directories(soeasy_schema PUBLIC
        third_party/sysroot/usr/include
    )
endif()
target_link_libraries(soeasy_schema PUBLIC
    capnp-rpc capnp kj-async kj
    $<$<BOOL:${WIN32}>:ws2_32>
)

# -------------------------------------------------------------
# 6. Storage Subsystem (SQLite3MC + FTS5 + cppjieba + Zstd)
# -------------------------------------------------------------
add_library(soeasy_storage STATIC
    src/storage/dat_jieba.cpp
    src/storage/fts5_jieba_tokenizer.cpp
    src/storage/reader_connection_pool.cpp
    src/storage/storage_engine.cpp
)
target_include_directories(soeasy_storage PUBLIC
    src
    third_party/sqlite3mc
    third_party/SQLiteCpp/include
    third_party/utfcpp/source
    third_party/cppjieba/include
    third_party/darts
    third_party/zstd/lib
)
target_link_libraries(soeasy_storage PUBLIC
    soeasy_common
    SQLiteCpp
    sqlite3mc
    libzstd_static
    pthread
)

add_executable(build_dat_dict
    tools/build_dat_dict.cpp
)
target_include_directories(build_dat_dict PRIVATE
    third_party/darts
)

# -------------------------------------------------------------
# 7. Worker Subsystem (L1 Text Parser, L2 Office Parser, L3 Image Parser & Worker Loop)
# -------------------------------------------------------------
add_library(soeasy_worker_lib STATIC
    third_party/miniz/miniz.c
    src/worker/archive_reader.cpp
    src/worker/l1_text_parser.cpp
    src/worker/l2_office_parser.cpp
    src/worker/l3_image_parser.cpp
    src/worker/parser_registry.cpp
    src/worker/ocr/image_filter.cpp
    src/worker/ocr/ppocr_v6_engine.cpp
    src/worker/ocr/ocr_engine_factory.cpp
    src/worker/worker_loop.cpp
)
target_include_directories(soeasy_worker_lib PUBLIC
    src
    schema
    third_party/miniz
    third_party/doctotext/include
    third_party/utfcpp/source
    third_party/onnxruntime/include
    ${OpenCV_INCLUDE_DIRS}
)
if(NOT WIN32)
    target_include_directories(soeasy_worker_lib PUBLIC
        third_party/sysroot/usr/include
    )
endif()
find_package(Iconv REQUIRED)
find_library(XML2_LIB NAMES xml2 libxml2.so.2 libxml2 REQUIRED)
find_library(Z_LIB NAMES z libz.so.1 libz REQUIRED)

if(WIN32)
    set(_SOEASY_LIBARCHIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libarchive/windows-x64")
else()
    set(_SOEASY_LIBARCHIVE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libarchive/linux-x86_64")
endif()

if(EXISTS "${_SOEASY_LIBARCHIVE_ROOT}/include/archive.h" AND EXISTS "${_SOEASY_LIBARCHIVE_ROOT}/lib/libarchive.a")
    message(STATUS "[LibArchive] Using prebuilt minimal static LibArchive from: ${_SOEASY_LIBARCHIVE_ROOT}")
    target_include_directories(soeasy_worker_lib PUBLIC "${_SOEASY_LIBARCHIVE_ROOT}/include")
    target_compile_definitions(soeasy_worker_lib PUBLIC LIBARCHIVE_STATIC)
    set(_SOEASY_LIBARCHIVE_LIBS "${_SOEASY_LIBARCHIVE_ROOT}/lib/libarchive.a")
    if(EXISTS "${_SOEASY_LIBARCHIVE_ROOT}/lib/libbz2.a")
        list(APPEND _SOEASY_LIBARCHIVE_LIBS "${_SOEASY_LIBARCHIVE_ROOT}/lib/libbz2.a")
    endif()
    find_library(_SOEASY_LZMA_LIB NAMES lzma liblzma.a liblzma REQUIRED)
    list(APPEND _SOEASY_LIBARCHIVE_LIBS "${_SOEASY_LZMA_LIB}" libzstd_static)
else()
    find_package(LibArchive REQUIRED)
    target_include_directories(soeasy_worker_lib PUBLIC ${LibArchive_INCLUDE_DIRS})
    set(_SOEASY_LIBARCHIVE_LIBS ${LibArchive_LIBRARIES})
endif()

target_link_libraries(soeasy_worker_lib PUBLIC
    soeasy_common
    soeasy_schema
    doctotext
    ${_SOEASY_LIBARCHIVE_LIBS}
    ${XML2_LIB}
    uchardet
    capnp
    kj
    ${Z_LIB}
    onnxruntime
    ${OpenCV_LIBS}
    Iconv::Iconv
)

add_executable(soeasy-worker
    src/worker/main_worker.cpp
)
target_link_libraries(soeasy-worker PRIVATE
    soeasy_worker_lib
)

# -------------------------------------------------------------
# 8. Service Subsystem (Watcher, Debouncer, NameIndex, Pipeline, Guard)
# -------------------------------------------------------------
add_library(soeasy_service STATIC
    src/service/path_filter_policy.cpp
    src/service/file_crawler.cpp
    src/service/watch_reconciler.cpp
    src/service/space_reclaimer.cpp
    src/service/index_config.cpp
    src/service/event_debouncer.cpp
    src/service/watch_engine.cpp
    src/service/name_index.cpp
    src/service/search_coordinator.cpp
    src/service/index_orchestrator.cpp
    src/service/index_pipeline.cpp
    src/service/worker_guard.cpp
    src/service/worker_process_utils.cpp
    src/service/worker_rpc_channel.cpp
    src/service/async_write_queue.cpp
    src/service/dictionary_service.cpp
)
target_include_directories(soeasy_service PUBLIC
    src
    schema
    third_party
    third_party/efsw/include
)
if(NOT WIN32)
    target_include_directories(soeasy_service PUBLIC
        third_party/sysroot/usr/include
    )
endif()
if(WIN32)
    target_link_libraries(soeasy_service PUBLIC
        soeasy_common
        soeasy_schema
        efsw-static
        capnp
        kj
        pthread
        psapi
        ws2_32
        Iconv::Iconv
    )
    target_link_libraries(soeasy_service PRIVATE
        soeasy_storage
    )
else()
    target_link_libraries(soeasy_service PUBLIC
        soeasy_common
        soeasy_schema
        efsw-static
        capnp
        kj
        pthread
        rt
    )
    target_link_libraries(soeasy_service PRIVATE
        soeasy_storage
    )
endif()

# -------------------------------------------------------------
# 9. Test Targets
# -------------------------------------------------------------
if(BUILD_TESTS)
  enable_testing()

  # Fetch GoogleTest & GoogleMock 1.14.0
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)

  macro(add_soeasy_test target_name timeout_sec)
    add_test(NAME ${target_name} COMMAND ${target_name})
    set(actual_timeout ${timeout_sec})
    if(WIN32)
      math(EXPR actual_timeout "${timeout_sec} * 3")
    endif()
    set_tests_properties(${target_name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        TIMEOUT ${actual_timeout}
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
  endmacro()

  # Phase 1 Test: Storage & FTS5
  add_executable(test_storage_engine
      tests/test_storage_engine.cpp
  )
  target_link_libraries(test_storage_engine PRIVATE
      soeasy_storage
  )
  add_soeasy_test(test_storage_engine 30)

  # Stage 2 Test: Reader Connection Pool & Concurrent Read/Write Under WAL
  add_executable(test_connection_pool
      tests/test_connection_pool.cpp
  )
  target_link_libraries(test_connection_pool PRIVATE
      soeasy_storage
      soeasy_common
  )
  add_soeasy_test(test_connection_pool 30)

  # Phase 2 Test: Sandbox & IPC
  add_executable(test_sandbox_ipc
      tests/test_sandbox_ipc.cpp
  )
  target_link_libraries(test_sandbox_ipc PRIVATE
      soeasy_service
      soeasy_worker_lib
      soeasy_storage
  )
  add_soeasy_test(test_sandbox_ipc 30)

  # Phase 3 Test: Watcher, Debouncer, NameIndex & Live Pipeline
  add_executable(test_watch_and_index
      tests/test_watch_and_index.cpp
  )
  target_link_libraries(test_watch_and_index PRIVATE
      soeasy_service
      soeasy_storage
      soeasy_worker_lib
  )
  add_soeasy_test(test_watch_and_index 30)
endif()

# -------------------------------------------------------------
# 10. GUI Subsystem (Qt5 Widgets Desktop Frontend)
# -------------------------------------------------------------
add_library(soeasy_unibreak STATIC
    third_party/libunibreak/unibreakbase.c
    third_party/libunibreak/unibreakdef.c
    third_party/libunibreak/linebreak.c
    third_party/libunibreak/linebreakdata.c
    third_party/libunibreak/linebreakdef.c
    third_party/libunibreak/eastasianwidthdef.c
    third_party/libunibreak/emojidef.c
)
target_include_directories(soeasy_unibreak PUBLIC
    third_party/libunibreak
)

add_library(soeasy_hyphen STATIC
    third_party/hyphen/hyphen.c
    third_party/hyphen/hnjalloc.c
)
target_include_directories(soeasy_hyphen PUBLIC
    third_party/hyphen
)

add_library(soeasy_gui STATIC
    src/gui/gui_utils.h
    src/gui/i18n.cpp
    src/gui/icon_provider.cpp
    src/gui/hover_preview_popup.cpp
    src/gui/document_viewer_widget.cpp
    src/gui/knuth_plass_viewer.cpp
    src/gui/typography_popover.cpp
    src/gui/typesetting/kp_engine.cpp
    src/gui/typesetting/cjk_typesetter.cpp
    src/gui/typesetting/preview_typography.cpp
    src/gui/content_preview_dialog.cpp
    src/gui/index_progress_dialog.cpp
    src/gui/parse_issues_dialog.cpp
    src/gui/preview_panel.cpp
    src/gui/settings/general_tab.cpp
    src/gui/settings/extensions_tab.cpp
    src/gui/settings/user_dict_tab.cpp
    src/gui/settings/performance_tab.cpp
    src/gui/settings_dialog.cpp
    src/gui/search_bar.cpp
    src/gui/search_table_model.cpp
    src/gui/result_view.cpp
    src/gui/title_bar.cpp
    src/gui/main_window.cpp
    resources/resources.qrc
)
target_include_directories(soeasy_gui PUBLIC
    src
)
target_link_libraries(soeasy_gui PUBLIC
    Qt5::Widgets
    Qt5::Gui
    Qt5::Core
    Qt5::Network
    soeasy_service
    soeasy_unibreak
    soeasy_hyphen
)

set(SOEASY_GUI_WIN32_RESOURCES "")
if(WIN32)
    set(SOEASY_GUI_WIN32_RESOURCES resources/soeasy.rc)
endif()

add_executable(soeasy-gui WIN32
    src/gui/main.cpp
    ${SOEASY_GUI_WIN32_RESOURCES}
)
target_link_libraries(soeasy-gui PRIVATE
    soeasy_gui
    soeasy_storage
)

# -------------------------------------------------------------
# 10.1 CLI Subsystem (Headless Unix CLI Binary)
# -------------------------------------------------------------
add_executable(soeasy-cli
    src/cli/main_cli.cpp
)
target_include_directories(soeasy-cli PRIVATE
    src
)
target_link_libraries(soeasy-cli PRIVATE
    soeasy_service
    soeasy_storage
    soeasy_common
)

if(BUILD_TESTS)
  # Phase 4 Test: Qt5 GUI Component Interaction & Smoke Test
  add_executable(test_gui_smoke
      tests/test_gui_smoke.cpp
  )
  target_link_libraries(test_gui_smoke PRIVATE
      soeasy_gui
      soeasy_storage
      Qt5::Widgets
      Qt5::Gui
      Qt5::Core
  )
  add_soeasy_test(test_gui_smoke 30)

  # Automated Product Hero Showcase Snapshot Generator
  add_executable(render_hero_shot
      tests/render_hero_shot.cpp
  )
  target_link_libraries(render_hero_shot PRIVATE
      soeasy_gui
      Qt5::Widgets
      Qt5::Gui
      Qt5::Core
  )

  # Real-world Live Search Snapshot Capture & Verification
  add_executable(capture_real_search_screenshot
      tests/capture_real_search_screenshot.cpp
  )
  target_link_libraries(capture_real_search_screenshot PRIVATE
      soeasy_gui
      soeasy_service
      soeasy_storage
      soeasy_common
      Qt5::Widgets
      Qt5::Gui
      Qt5::Core
  )

  # Stage 4 Performance & Scale Test: 10,000+ Viewport Scroll, Snippet & Memory Stress
  add_executable(test_large_scale_perf
      tests/test_large_scale_perf.cpp
  )
  target_link_libraries(test_large_scale_perf PRIVATE
      soeasy_gui
      soeasy_storage
      soeasy_common
      Qt5::Widgets
      Qt5::Gui
      Qt5::Core
  )
  add_soeasy_test(test_large_scale_perf 45)

  # Phase 5 Test: L2 Office Document Parser (OOXML, PDF, Sandbox & FTS5)
  add_executable(test_office_parser
      tests/test_office_parser.cpp
  )
  target_include_directories(test_office_parser PRIVATE
      src
      third_party/miniz
  )
  target_link_libraries(test_office_parser PRIVATE
      soeasy_worker_lib
      soeasy_service
      soeasy_storage
  )
  add_soeasy_test(test_office_parser 30)

  # Architecture Refactoring Test: Worker Parser Polymorphism & ParserRegistry (Stage 1)
  add_executable(test_parser_registry
      tests/test_parser_registry.cpp
  )
  target_include_directories(test_parser_registry PRIVATE
      src
  )
  target_link_libraries(test_parser_registry PRIVATE
      soeasy_worker_lib
      soeasy_service
      soeasy_storage
  )
  add_soeasy_test(test_parser_registry 30)

  # Architecture Refactoring Test: Mid-tier CQRS SearchCoordinator (Stage 3)
  add_executable(test_search_coordinator
      tests/test_search_coordinator.cpp
  )
  target_include_directories(test_search_coordinator PRIVATE
      src
  )
  target_link_libraries(test_search_coordinator PRIVATE
      soeasy_service
      soeasy_storage
      soeasy_common
  )
  add_soeasy_test(test_search_coordinator 30)

  # Test: spdlog Logging & Index Rebuild Verification
  add_executable(test_logging_and_rebuild
      tests/test_logging_and_rebuild.cpp
  )
  target_link_libraries(test_logging_and_rebuild PRIVATE
      soeasy_service
      soeasy_worker_lib
      soeasy_storage
      soeasy_common
  )
  add_soeasy_test(test_logging_and_rebuild 30)

  # Test: L3 Image OCR Parser & Scientific Filtering
  add_executable(test_image_ocr
      tests/test_image_ocr.cpp
  )
  target_link_libraries(test_image_ocr PRIVATE
      soeasy_worker_lib
      soeasy_service
      soeasy_storage
      soeasy_common
  )
  add_soeasy_test(test_image_ocr 30)

  # Comprehensive Matrix Verification: Idle cycle, scaling, large/small files, crash recovery
  add_executable(test_comprehensive_matrix
      tests/test_comprehensive_matrix.cpp
  )
  target_link_libraries(test_comprehensive_matrix PRIVATE
      soeasy_worker_lib
      soeasy_service
      soeasy_storage
      soeasy_common
  )
  add_soeasy_test(test_comprehensive_matrix 45)

  # Test: DAT Jieba & Overlay User Dict Test Suite
  add_executable(test_dat_jieba
      tests/test_dat_jieba.cpp
  )
  target_link_libraries(test_dat_jieba PRIVATE
      soeasy_storage
      soeasy_common
  )
  add_soeasy_test(test_dat_jieba 30)

  # Test: System Environment & Resource Limits Optimization
  add_executable(test_system_env
      tests/test_system_env.cpp
  )
  target_link_libraries(test_system_env PRIVATE
      soeasy_common
  )
  add_soeasy_test(test_system_env 15)

  # Test & Benchmark: Investigation of Log Printing Impact on Parser
  add_executable(test_log_impact
      tests/test_log_impact.cpp
  )
  target_link_libraries(test_log_impact PRIVATE
      soeasy_worker_lib
      soeasy_service
      soeasy_storage
      soeasy_common
  )
  add_soeasy_test(test_log_impact 30)

  # Stage 4 GoogleTest & GoogleMock Modern Test Suite: Storage Engine & Transaction RAII Safety
  add_executable(test_storage_transaction_gtest
      tests/test_storage_transaction_gtest.cpp
  )
  target_include_directories(test_storage_transaction_gtest PRIVATE
      src
  )
  target_link_libraries(test_storage_transaction_gtest PRIVATE
      soeasy_storage
      soeasy_common
      GTest::gtest
      GTest::gtest_main
      GTest::gmock
  )
  gtest_discover_tests(test_storage_transaction_gtest
      DISCOVERY_MODE PRE_TEST
      PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 30
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  # Modern Unit Test Suite: Common Snippet Highlighter & FTS Keyword Extraction Boundary Matrix
  add_executable(test_snippet_highlighter_gtest
      tests/test_snippet_highlighter_gtest.cpp
  )
  target_include_directories(test_snippet_highlighter_gtest PRIVATE
      src
      third_party/utfcpp/source
  )
  target_link_libraries(test_snippet_highlighter_gtest PRIVATE
      soeasy_common
      GTest::gtest
      GTest::gtest_main
  )
  gtest_discover_tests(test_snippet_highlighter_gtest
      DISCOVERY_MODE PRE_TEST
      PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 10
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  # Modern Unit Test Suite: FTS5 Syntax Matrix, Boolean, Column, NEAR, & Graceful Fallback
  add_executable(test_fts5_syntax_gtest
      tests/test_fts5_syntax_gtest.cpp
  )
  target_include_directories(test_fts5_syntax_gtest PRIVATE
      src
      third_party/sqlite3mc
      third_party/utfcpp/source
  )
  target_link_libraries(test_fts5_syntax_gtest PRIVATE
      soeasy_storage
      soeasy_common
      GTest::gtest
      GTest::gtest_main
  )
  gtest_discover_tests(test_fts5_syntax_gtest
      DISCOVERY_MODE PRE_TEST
      PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 20
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  # Modern Unit Test Suite: SQLiteCpp integration, AES-128 physical encryption, and FTS5 tokenization
  add_executable(test_sqlitecpp_gtest
      tests/test_sqlitecpp_gtest.cpp
  )
  target_include_directories(test_sqlitecpp_gtest PRIVATE
      src
      third_party/sqlite3mc
      third_party/SQLiteCpp/include
      third_party/utfcpp/source
  )
  target_link_libraries(test_sqlitecpp_gtest PRIVATE
      soeasy_storage
      soeasy_common
      SQLiteCpp
      sqlite3mc
      GTest::gtest
      GTest::gtest_main
  )
  gtest_discover_tests(test_sqlitecpp_gtest
      DISCOVERY_MODE PRE_TEST
      PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 20
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  # Modern Unit Test Suite: Machine-Bound Encryption & Zero-Friction Lifecycle Key Provider
  add_executable(test_machine_key_provider_gtest
      tests/test_machine_key_provider_gtest.cpp
  )
  target_include_directories(test_machine_key_provider_gtest PRIVATE
      src
      third_party/sqlite3mc
      third_party/SQLiteCpp/include
      third_party/utfcpp/source
      third_party/spdlog/include
  )
  target_link_libraries(test_machine_key_provider_gtest PRIVATE
      soeasy_storage
      soeasy_common
      SQLiteCpp
      sqlite3mc
      GTest::gtest
      GTest::gtest_main
  )
  gtest_discover_tests(test_machine_key_provider_gtest
      DISCOVERY_MODE PRE_TEST
      PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 20
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  # Modern Unit Test & Benchmark Suite: SoA + Arena NameIndex
  add_executable(test_name_index_gtest
      tests/test_name_index_gtest.cpp
  )
  target_include_directories(test_name_index_gtest PRIVATE
      src
  )
  target_link_libraries(test_name_index_gtest PRIVATE
      soeasy_service
      soeasy_storage
      soeasy_common
      GTest::gtest
      GTest::gtest_main
  )
  gtest_discover_tests(test_name_index_gtest
      DISCOVERY_MODE PRE_TEST
      PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 30
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  # State Machine Test Suite: Config Diff, Prune, Watch-First Rebuild Reconcile, Lock/Meta
  add_executable(test_index_state_machine_gtest
      tests/test_index_state_machine_gtest.cpp
  )
  target_include_directories(test_index_state_machine_gtest PRIVATE
      src
      .
  )
  target_link_libraries(test_index_state_machine_gtest PRIVATE
      soeasy_service
      soeasy_storage
      soeasy_common
      GTest::gtest
      GTest::gtest_main
  )
  gtest_discover_tests(test_index_state_machine_gtest
      DISCOVERY_MODE PRE_TEST
      PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 60
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  # ArchiveReader Unit, Nested Extraction & End-to-End Orchestrator Archive Lifecycle Tests
  add_executable(test_archive_reader_gtest
      tests/test_archive_reader_gtest.cpp
  )
  target_include_directories(test_archive_reader_gtest PRIVATE
      src
      .
  )
  target_link_libraries(test_archive_reader_gtest PRIVATE
      soeasy_worker_lib
      soeasy_service
      soeasy_storage
      soeasy_common
      GTest::gtest
      GTest::gtest_main
  )
  gtest_discover_tests(test_archive_reader_gtest
      DISCOVERY_MODE PRE_TEST
      PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      TIMEOUT 60
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
  )

  # Knuth-Plass Line-Breaking & CJK/Latin Typesetting Unit Tests
  add_executable(test_knuth_plass
      tests/test_knuth_plass.cpp
  )
  target_link_libraries(test_knuth_plass PRIVATE
      soeasy_gui
      Qt5::Widgets
      Qt5::Gui
      Qt5::Core
  )
  add_soeasy_test(test_knuth_plass 30)

  # Knuth-Plass Viewer Widget, Mode Routing & Performance Tests
  add_executable(test_knuth_plass_viewer
      tests/test_knuth_plass_viewer.cpp
  )
  target_link_libraries(test_knuth_plass_viewer PRIVATE
      soeasy_gui
      soeasy_common
      Qt5::Widgets
      Qt5::Gui
      Qt5::Core
  )
  add_soeasy_test(test_knuth_plass_viewer 30)
endif()

# -------------------------------------------------------------
# 11. Packaging Target (One-click Standalone Zip Generation)
# -------------------------------------------------------------
add_custom_target(package_zip
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
    DEPENDS soeasy-gui soeasy-worker soeasy-cli
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building and packaging SoEasy standalone release zip..."
)
