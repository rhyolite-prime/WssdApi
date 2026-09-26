#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Sapo::sapo_core" for configuration "Release"
set_property(TARGET Sapo::sapo_core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Sapo::sapo_core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libsapo_core.a"
  )

list(APPEND _cmake_import_check_targets Sapo::sapo_core )
list(APPEND _cmake_import_check_files_for_Sapo::sapo_core "${_IMPORT_PREFIX}/lib/libsapo_core.a" )

# Import target "Sapo::sapoc" for configuration "Release"
set_property(TARGET Sapo::sapoc APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Sapo::sapoc PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/sapoc"
  )

list(APPEND _cmake_import_check_targets Sapo::sapoc )
list(APPEND _cmake_import_check_files_for_Sapo::sapoc "${_IMPORT_PREFIX}/bin/sapoc" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
