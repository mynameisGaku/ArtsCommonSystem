cmake_minimum_required(VERSION 3.24)
project(ACS LANGUAGES CXX VERSION 0.1.0)

if(NOT WIN32)
    message(FATAL_ERROR "ACS currently targets Windows only")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ACSCompilerOptions)
include(ACSModuleSystem)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACS_BUILD_TESTS    "Build ACS unit tests"               ON)
option(ACS_ENABLE_ASSERTS "Enable ACS_ASSERT in all builds"    ON)

set(ACS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---- 1. Discover everything available under src/<mod>/Module.cmake -------
acs_discover_modules("${ACS_SOURCE_ROOT}")

# ---- 2. User picks what to build ----------------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/modules.cmake")

# ---- 3. Validate, create targets, apply WITH_ACS_* defines --------------
acs_finalize_modules()

if(ACS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
