include(GNUInstallDirs)

if(NOT TARGET PadDrawBoard)
  message(FATAL_ERROR "PadDrawBoard target must exist before packaging is configured")
endif()

if(NOT WIN32)
  message(FATAL_ERROR "PadDrawBoard packaging currently supports Windows only")
endif()

install(TARGETS PadDrawBoard
  RUNTIME DESTINATION .
  COMPONENT runtime
)

# MinGW executables use the GCC and C++ runtimes, which are not present on a
# clean Windows installation.  Resolve them from the exact compiler selected
# by CMake so packaging cannot silently pick up DLLs from an unrelated PATH
# entry.  Keep this branch isolated from MSVC packaging: MSVC runtime handling
# remains owned by InstallRequiredSystemLibraries below.
if(MINGW)
  if(NOT EXISTS "${CMAKE_CXX_COMPILER}")
    message(FATAL_ERROR
      "MinGW packaging cannot locate the configured C++ compiler: ${CMAKE_CXX_COMPILER}")
  endif()

  get_filename_component(_pdb_mingw_compiler_dir
    "${CMAKE_CXX_COMPILER}" DIRECTORY)
  set(_pdb_mingw_runtime_dlls)
  foreach(_pdb_mingw_runtime IN ITEMS libgcc_s_seh-1.dll libstdc++-6.dll)
    execute_process(
      COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${_pdb_mingw_runtime}"
      RESULT_VARIABLE _pdb_runtime_result
      OUTPUT_VARIABLE _pdb_runtime_path
      ERROR_VARIABLE _pdb_runtime_error
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_STRIP_TRAILING_WHITESPACE
    )

    set(_pdb_runtime_candidate "${_pdb_runtime_path}")
    if(_pdb_runtime_result EQUAL 0
       AND IS_ABSOLUTE "${_pdb_runtime_candidate}"
       AND EXISTS "${_pdb_runtime_candidate}")
      # The compiler reported an existing absolute path.  Use it verbatim.
    else()
      # Some MinGW wrappers return only the filename.  The compiler's own bin
      # directory is the only fallback accepted; never search PATH globally.
      set(_pdb_runtime_candidate
        "${_pdb_mingw_compiler_dir}/${_pdb_mingw_runtime}")
    endif()

    if(NOT EXISTS "${_pdb_runtime_candidate}"
       OR NOT IS_ABSOLUTE "${_pdb_runtime_candidate}")
      message(FATAL_ERROR
        "MinGW packaging requires ${_pdb_mingw_runtime} beside PadDrawBoard.exe, "
        "but it was not found through ${CMAKE_CXX_COMPILER} or its compiler bin "
        "directory (${_pdb_mingw_compiler_dir}). "
        "Compiler output: ${_pdb_runtime_path} ${_pdb_runtime_error}")
    endif()

    list(APPEND _pdb_mingw_runtime_dlls "${_pdb_runtime_candidate}")
  endforeach()

  install(FILES ${_pdb_mingw_runtime_dlls}
    DESTINATION .
    COMPONENT runtime
  )
endif()

install(FILES
  "${PROJECT_SOURCE_DIR}/LICENSE"
  "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  DESTINATION .
  COMPONENT runtime
)

if(PDB_PACKAGE_BUNDLED_ADB)
  if(NOT PDB_BUNDLED_ADB_DIR)
    message(FATAL_ERROR
      "PDB_PACKAGE_BUNDLED_ADB is ON but PDB_BUNDLED_ADB_DIR is empty")
  endif()

  get_filename_component(PDB_BUNDLED_ADB_DIR_ABS
    "${PDB_BUNDLED_ADB_DIR}" ABSOLUTE)
  foreach(_pdb_adb_required IN ITEMS adb.exe NOTICE.txt source.properties)
    if(NOT EXISTS "${PDB_BUNDLED_ADB_DIR_ABS}/${_pdb_adb_required}")
      message(FATAL_ERROR
        "The Platform Tools directory is incomplete: missing ${_pdb_adb_required}")
    endif()
  endforeach()

  # Copy the complete official directory, including Google's NOTICE.txt and
  # every accompanying third-party license, without filtering or renaming it.
  install(DIRECTORY "${PDB_BUNDLED_ADB_DIR_ABS}/"
    DESTINATION tools/adb/36.0.2
    COMPONENT runtime
  )
else()
  message(STATUS
    "Bundled Platform Tools are disabled; set PDB_PACKAGE_BUNDLED_ADB=ON for release packaging")
endif()

if(WIN32)
  include(InstallRequiredSystemLibraries)
endif()

set(CPACK_PACKAGE_NAME "PadDrawBoard")
set(CPACK_PACKAGE_VENDOR "PadDrawBoard contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Open-source low-latency Xiaomi Pad pen display for Windows Ink")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "PadDrawBoard")
set(CPACK_PACKAGE_FILE_NAME
  "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES OFF)
set(CPACK_GENERATOR "ZIP;NSIS")

set(CPACK_NSIS_DISPLAY_NAME "PadDrawBoard")
set(CPACK_NSIS_PACKAGE_NAME "PadDrawBoard")
set(CPACK_NSIS_INSTALLER_ARCHITECTURE "x64")
set(CPACK_NSIS_MODIFY_PATH OFF)
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
set(CPACK_NSIS_MENU_LINKS "PadDrawBoard.exe" "PadDrawBoard")

include(CPack)

# These explicit targets make the two release formats discoverable from the
# build tree.  The packaging script uses the same CPack configuration while
# selecting its output directory and enforcing the bundled-ADB precondition.
if(CMAKE_CONFIGURATION_TYPES)
  set(PDB_CPACK_CONFIG_ARGS -C Release)
else()
  set(PDB_CPACK_CONFIG_ARGS)
endif()

add_custom_target(package-zip
  COMMAND "${CMAKE_CPACK_COMMAND}"
    --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
    -G ZIP
    ${PDB_CPACK_CONFIG_ARGS}
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Creating the PadDrawBoard Windows x64 ZIP"
  VERBATIM
)

add_custom_target(package-nsis
  COMMAND "${CMAKE_CPACK_COMMAND}"
    --config "${CMAKE_BINARY_DIR}/CPackConfig.cmake"
    -G NSIS
    ${PDB_CPACK_CONFIG_ARGS}
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Creating the PadDrawBoard Windows x64 NSIS installer"
  VERBATIM
)
