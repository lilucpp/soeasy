#----------------------------------------------------------------
# Generated CMake target import file for configuration "None".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "uchardet::libuchardet" for configuration "None"
set_property(TARGET uchardet::libuchardet APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(uchardet::libuchardet PROPERTIES
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libuchardet.so.0.0.8"
  IMPORTED_SONAME_NONE "libuchardet.so.0"
  )

list(APPEND _cmake_import_check_targets uchardet::libuchardet )
list(APPEND _cmake_import_check_files_for_uchardet::libuchardet "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libuchardet.so.0.0.8" )

# Import target "uchardet::libuchardet_static" for configuration "None"
set_property(TARGET uchardet::libuchardet_static APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(uchardet::libuchardet_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NONE "CXX"
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libuchardet.a"
  )

list(APPEND _cmake_import_check_targets uchardet::libuchardet_static )
list(APPEND _cmake_import_check_files_for_uchardet::libuchardet_static "${_IMPORT_PREFIX}/lib/x86_64-linux-gnu/libuchardet.a" )

# Import target "uchardet::uchardet" for configuration "None"
set_property(TARGET uchardet::uchardet APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(uchardet::uchardet PROPERTIES
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/bin/uchardet"
  )

list(APPEND _cmake_import_check_targets uchardet::uchardet )
list(APPEND _cmake_import_check_files_for_uchardet::uchardet "${_IMPORT_PREFIX}/bin/uchardet" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
