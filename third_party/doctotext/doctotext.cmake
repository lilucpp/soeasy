# ==============================================================================
# DocToText Prebuilt Binary Dependency Manager
# Downloads and extracts prebuilt doctotext binaries from GitHub Releases
# ==============================================================================

set(DOCTOTEXT_VERSION "v5.1.6")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(DOCTOTEXT_ARCH "linux-x86_64")
        set(DOCTOTEXT_ARCHIVE "doctotext-linux-x86_64.tar.gz")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(DOCTOTEXT_ARCH "linux-arm64")
        set(DOCTOTEXT_ARCHIVE "doctotext-linux-arm64.tar.gz")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i.86|x86")
        set(DOCTOTEXT_ARCH "linux-x86")
        set(DOCTOTEXT_ARCHIVE "doctotext-linux-x86.tar.gz")
    else()
        message(FATAL_ERROR "Unsupported Linux architecture for doctotext: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(DOCTOTEXT_ARCH "windows-x64")
        set(DOCTOTEXT_ARCHIVE "doctotext-windows-x64.zip")
    else()
        set(DOCTOTEXT_ARCH "windows-x86")
        set(DOCTOTEXT_ARCHIVE "doctotext-windows-x86.zip")
    endif()
else()
    message(FATAL_ERROR "Unsupported OS for doctotext: ${CMAKE_SYSTEM_NAME}")
endif()

set(DOCTOTEXT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/doctotext")
set(DOCTOTEXT_LIB_DIR "${DOCTOTEXT_DIR}/lib")
set(DOCTOTEXT_INCLUDE_DIR "${DOCTOTEXT_DIR}/include")
set(DOCTOTEXT_STAMP "${DOCTOTEXT_DIR}/.installed_${DOCTOTEXT_VERSION}_${DOCTOTEXT_ARCH}")

option(DOCTOTEXT_USE_LOCAL "Use locally populated DocToText libraries/headers without downloading" OFF)
set(DOCTOTEXT_LOCAL_DIR "" CACHE PATH "Optional local DocToText build output directory (e.g. ../doctotext/build) for atomic header+lib sync when DOCTOTEXT_USE_LOCAL=ON")

if(NOT EXISTS "${DOCTOTEXT_STAMP}" AND DOCTOTEXT_USE_LOCAL)
    if(NOT "${DOCTOTEXT_LOCAL_DIR}" STREQUAL "")
        if(NOT EXISTS "${DOCTOTEXT_LOCAL_DIR}/doctotext_c_api.h")
            message(FATAL_ERROR "DOCTOTEXT_LOCAL_DIR='${DOCTOTEXT_LOCAL_DIR}' does not contain doctotext_c_api.h")
        endif()
        file(MAKE_DIRECTORY "${DOCTOTEXT_LIB_DIR}")
        file(MAKE_DIRECTORY "${DOCTOTEXT_INCLUDE_DIR}")
        file(GLOB _LOCAL_LIBS
            "${DOCTOTEXT_LOCAL_DIR}/*.so*"
            "${DOCTOTEXT_LOCAL_DIR}/*.a"
            "${DOCTOTEXT_LOCAL_DIR}/*.dll"
        )
        foreach(_lib ${_LOCAL_LIBS})
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${_lib}" "${DOCTOTEXT_LIB_DIR}/" RESULT_VARIABLE _COPY_RET)
            if(NOT _COPY_RET EQUAL 0)
                message(FATAL_ERROR "Failed to copy DocToText library ${_lib} to ${DOCTOTEXT_LIB_DIR}")
            endif()
        endforeach()
        file(GLOB _LOCAL_HEADERS "${DOCTOTEXT_LOCAL_DIR}/*.h")
        foreach(_hdr ${_LOCAL_HEADERS})
            execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${_hdr}" "${DOCTOTEXT_INCLUDE_DIR}/" RESULT_VARIABLE _COPY_RET)
            if(NOT _COPY_RET EQUAL 0)
                message(FATAL_ERROR "Failed to copy DocToText header ${_hdr} to ${DOCTOTEXT_INCLUDE_DIR}")
            endif()
        endforeach()
    endif()

    if(NOT EXISTS "${DOCTOTEXT_INCLUDE_DIR}/doctotext_c_api.h")
        message(FATAL_ERROR "DOCTOTEXT_USE_LOCAL=ON but doctotext_c_api.h not found in ${DOCTOTEXT_INCLUDE_DIR}")
    endif()
    if(WIN32)
        if(NOT EXISTS "${DOCTOTEXT_LIB_DIR}/doctotext.dll" OR NOT EXISTS "${DOCTOTEXT_LIB_DIR}/libdoctotext.a")
            message(FATAL_ERROR "DOCTOTEXT_USE_LOCAL=ON but doctotext.dll/libdoctotext.a not found in ${DOCTOTEXT_LIB_DIR}")
        endif()
    elseif(UNIX)
        if(NOT EXISTS "${DOCTOTEXT_LIB_DIR}/libdoctotext.so")
            message(FATAL_ERROR "DOCTOTEXT_USE_LOCAL=ON but libdoctotext.so not found in ${DOCTOTEXT_LIB_DIR}")
        endif()
    endif()
    message(STATUS "[DocToText] Using locally populated ${DOCTOTEXT_VERSION} (${DOCTOTEXT_ARCH}) artifacts in ${DOCTOTEXT_DIR} (DOCTOTEXT_USE_LOCAL=ON)")
    file(WRITE "${DOCTOTEXT_STAMP}" "${DOCTOTEXT_VERSION} ${DOCTOTEXT_ARCH} (local)\n")
endif()

if(NOT EXISTS "${DOCTOTEXT_STAMP}")
    message(STATUS "[DocToText] Downloading prebuilt ${DOCTOTEXT_VERSION} for ${DOCTOTEXT_ARCH}...")
    set(DOCTOTEXT_URL "https://github.com/lilucpp/doctotext/releases/download/${DOCTOTEXT_VERSION}/${DOCTOTEXT_ARCHIVE}")
    set(DOCTOTEXT_DOWNLOAD_FILE "${CMAKE_BINARY_DIR}/${DOCTOTEXT_ARCHIVE}")

    file(DOWNLOAD "${DOCTOTEXT_URL}" "${DOCTOTEXT_DOWNLOAD_FILE}"
        SHOW_PROGRESS
        STATUS DL_STATUS
        TIMEOUT 60
    )
    list(GET DL_STATUS 0 DL_RET)
    if(NOT DL_RET EQUAL 0)
        list(GET DL_STATUS 1 DL_MSG)
        message(FATAL_ERROR "Failed to download DocToText from ${DOCTOTEXT_URL}: ${DL_MSG}")
    endif()

    message(STATUS "[DocToText] Extracting ${DOCTOTEXT_ARCHIVE}...")
    set(DOCTOTEXT_TEMP_EXTRACT "${CMAKE_BINARY_DIR}/doctotext_unpack")
    file(REMOVE_RECURSE "${DOCTOTEXT_TEMP_EXTRACT}")
    file(MAKE_DIRECTORY "${DOCTOTEXT_TEMP_EXTRACT}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xvf "${DOCTOTEXT_DOWNLOAD_FILE}"
        WORKING_DIRECTORY "${DOCTOTEXT_TEMP_EXTRACT}"
        RESULT_VARIABLE EXTRACT_RET
    )
    if(NOT EXTRACT_RET EQUAL 0)
        message(FATAL_ERROR "Failed to unpack DocToText archive: ${DOCTOTEXT_DOWNLOAD_FILE}")
    endif()

    file(MAKE_DIRECTORY "${DOCTOTEXT_LIB_DIR}")
    file(MAKE_DIRECTORY "${DOCTOTEXT_INCLUDE_DIR}")

    # Copy libraries (*.so*, *.a, *.dll)
    file(GLOB_RECURSE FOUND_LIBS
        "${DOCTOTEXT_TEMP_EXTRACT}/*.so*"
        "${DOCTOTEXT_TEMP_EXTRACT}/*.a"
        "${DOCTOTEXT_TEMP_EXTRACT}/*.dll"
    )
    foreach(lib_file ${FOUND_LIBS})
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${lib_file}" "${DOCTOTEXT_LIB_DIR}/")
    endforeach()

    # Copy headers (*.h)
    file(GLOB_RECURSE FOUND_HEADERS "${DOCTOTEXT_TEMP_EXTRACT}/*.h")
    foreach(hdr_file ${FOUND_HEADERS})
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${hdr_file}" "${DOCTOTEXT_INCLUDE_DIR}/")
    endforeach()

    file(WRITE "${DOCTOTEXT_STAMP}" "${DOCTOTEXT_VERSION} ${DOCTOTEXT_ARCH}\n")
    message(STATUS "[DocToText] Installed successfully into ${DOCTOTEXT_DIR}")
endif()
