cmake_minimum_required(VERSION 3.22)

project(PadDrawBoard VERSION 0.1.0 LANGUAGES CXX)

option(PDB_BUILD_TESTS "Build PadDrawBoard native tests" ON)
option(PDB_ENABLE_PACKAGING "Enable install and CPack packaging rules" ON)
option(PDB_PACKAGE_BUNDLED_ADB "Include the verified Platform Tools bundle in installs" OFF)
set(PDB_BUNDLED_ADB_DIR "" CACHE PATH
    "Extracted platform-tools directory to install as tools/adb/36.0.2")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "PadDrawBoard desktop currently supports Windows 11 x64 only")
endif()

if(PDB_BUILD_TESTS)
  include(CTest)
endif()

add_subdirectory(desktop)

if(PDB_ENABLE_PACKAGING)
  include("${CMAKE_SOURCE_DIR}/cmake/PadDrawBoardPackaging.cmake")
endif()
