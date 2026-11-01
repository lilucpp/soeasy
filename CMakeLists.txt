cmake_minimum_required(VERSION 3.16)
project(soeasy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)
find_package(OpenCV REQUIRED)

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

# ONNX Runtime Windows dependency download if missing
if(WIN32 AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/onnxruntime.lib")
    message(STATUS "[ONNXRuntime] Downloading official Microsoft ONNX Runtime Windows x64 v1.20.0...")
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v1.20.0/onnxruntime-win-x64-1.20.0.zip")
    set(ORT_ZIP "${CMAKE_BINARY_DIR}/onnxruntime-win-x64-1.20.0.zip")
    file(DOWNLOAD "${ORT_URL}" "${ORT_ZIP}" SHOW_PROGRESS STATUS ORT_DL_STATUS TIMEOUT 120)
    list(GET ORT_DL_STATUS 0 ORT_DL_RET)
    if(ORT_DL_RET EQUAL 0)
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ort_unpack")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E tar xvf "${ORT_ZIP}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/ort_unpack"
        )
        file(GLOB_RECURSE ORT_FOUND_LIBS
            "${CMAKE_BINARY_DIR}/ort_unpack/*.lib"
            "${CMAKE_BINARY_DIR}/ort_unpack/*.dll"
        )
        foreach(f ${ORT_FOUND_LIBS})
            file(COPY "${f}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/lib/")
        endforeach()
        message(STATUS "[ONNXRuntime] Windows x64 binaries installed successfully.")
    else()
        message(WARNING "[ONNXRuntime] Failed to download Windows ONNX Runtime binaries.")
    endif()
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
if(WIN32)
    target_link_libraries(sqlite3mc PRIVATE pthread)
else()
    target_link_libraries(sqlite3mc PRIVATE dl pthread)
endif()

# -------------------------------------------------------------
# 3. Third-party: efsw (Entropia File System Watcher)
# -------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TEST_APP OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/efsw ${CMAKE_BINARY_DIR}/efsw_build EXCLUDE_FROM_ALL)

# -------------------------------------------------------------
# 4. Common Library (SHM Slot Pool + spdlog Logging)
# -------------------------------------------------------------
add_library(soeasy_common STATIC
    src/common/shm_pool.cpp
    src/common/logger.cpp
)
target_include_directories(soeasy_common PUBLIC
    src
    third_party/spdlog/include
)
if(WIN32)
    target_link_libraries(soeasy_common PUBLIC pthread)
else()
    target_link_libraries(soeasy_common PUBLIC rt pthread)
endif()

# -------------------------------------------------------------
# 5. Cap'n Proto Schema Library
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
    capnp kj
)

# -------------------------------------------------------------
# 6. Storage Subsystem (SQLite3MC + FTS5 + cppjieba + Zstd)
# -------------------------------------------------------------
add_library(soeasy_storage STATIC
    src/storage/fts5_jieba_tokenizer.cpp
    src/storage/storage_engine.cpp
)
target_include_directories(soeasy_storage PUBLIC
    src
    third_party/sqlite3mc
    third_party/utfcpp/source
    third_party/cppjieba/include
    third_party/zstd/lib
)
target_link_libraries(soeasy_storage PUBLIC
    sqlite3mc
    libzstd_static
    pthread
)

# -------------------------------------------------------------
# 7. Worker Subsystem (L1 Text Parser, L2 Office Parser, L3 Image Parser & Worker Loop)
# -------------------------------------------------------------
add_library(soeasy_worker_lib STATIC
    third_party/miniz/miniz.c
    src/worker/l1_text_parser.cpp
    src/worker/l2_office_parser.cpp
    src/worker/l3_image_parser.cpp
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

target_link_libraries(soeasy_worker_lib PUBLIC
    soeasy_common
    soeasy_schema
    doctotext
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
    src/service/index_config.cpp
    src/service/event_debouncer.cpp
    src/service/watch_engine.cpp
    src/service/name_index.cpp
    src/service/doc_prechecker.cpp
    src/service/index_pipeline.cpp
    src/service/worker_guard.cpp
    src/service/async_write_queue.cpp
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
        soeasy_worker_lib
        soeasy_storage
        efsw-static
        capnp
        kj
        pthread
        psapi
        ws2_32
        Iconv::Iconv
    )
else()
    target_link_libraries(soeasy_service PUBLIC
        soeasy_common
        soeasy_schema
        soeasy_worker_lib
        soeasy_storage
        efsw-static
        capnp
        kj
        pthread
        rt
    )
endif()

# -------------------------------------------------------------
# 9. Test Targets
# -------------------------------------------------------------
if(BUILD_TESTS)
  # Phase 1 Test: Storage & FTS5
  add_executable(test_storage_engine
      tests/test_storage_engine.cpp
  )
  target_link_libraries(test_storage_engine PRIVATE
      soeasy_storage
  )

  # Phase 2 Test: Sandbox & IPC
  add_executable(test_sandbox_ipc
      tests/test_sandbox_ipc.cpp
  )
  target_link_libraries(test_sandbox_ipc PRIVATE
      soeasy_service
      soeasy_worker_lib
      soeasy_storage
  )

  # Phase 3 Test: Watcher, Debouncer, NameIndex & Live Pipeline
  add_executable(test_watch_and_index
      tests/test_watch_and_index.cpp
  )
  target_link_libraries(test_watch_and_index PRIVATE
      soeasy_service
      soeasy_storage
      soeasy_worker_lib
  )
endif()

# -------------------------------------------------------------
# 10. GUI Subsystem (Qt5 Widgets Desktop Frontend)
# -------------------------------------------------------------
add_library(soeasy_gui STATIC
    src/gui/i18n.cpp
    src/gui/icon_provider.cpp
    src/gui/hover_preview_popup.cpp
    src/gui/content_preview_dialog.cpp
    src/gui/preview_panel.cpp
    src/gui/settings_dialog.cpp
    src/gui/search_bar.cpp
    src/gui/result_view.cpp
    src/gui/title_bar.cpp
    src/gui/main_window.cpp
)
target_include_directories(soeasy_gui PUBLIC
    src
)
target_link_libraries(soeasy_gui PUBLIC
    Qt5::Widgets
    Qt5::Gui
    Qt5::Core
    soeasy_service
    soeasy_storage
)

add_executable(soeasy-gui
    src/gui/main.cpp
)
target_link_libraries(soeasy-gui PRIVATE
    soeasy_gui
)

if(BUILD_TESTS)
  # Phase 4 Test: Qt5 GUI Component Interaction & Smoke Test
  add_executable(test_gui_smoke
      tests/test_gui_smoke.cpp
  )
  target_link_libraries(test_gui_smoke PRIVATE
      soeasy_gui
      Qt5::Widgets
      Qt5::Gui
      Qt5::Core
  )

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
endif()

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

# -------------------------------------------------------------
# 11. Packaging Target (One-click Standalone Zip Generation)
# -------------------------------------------------------------
add_custom_target(package_zip
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/package.sh
    DEPENDS soeasy-gui soeasy-worker
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building and packaging SoEasy standalone release zip..."
)
