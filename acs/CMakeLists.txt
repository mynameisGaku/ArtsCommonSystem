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
option(ACS_BUILD_SAMPLES  "Build sample applications"          ON)
option(ACS_ENABLE_ASSERTS "Enable ACS_ASSERT in all builds"    ON)

set(ACS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---- 1. src/<mod>/Module.cmake から利用可能なモジュールを発見する ---------
acs_discover_modules("${ACS_SOURCE_ROOT}")

# ---- 2. ユーザーが modules.cmake で有効化する -----------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/modules.cmake")

# ---- 3. 依存関係を検証して target を生成、WITH_ACS_* define を付与 -------
acs_finalize_modules()

if(ACS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ACS_BUILD_SAMPLES)
    add_subdirectory(samples/HelloWindow)
    add_subdirectory(samples/HelloTriangle)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloImGui/CMakeLists.txt")
        add_subdirectory(samples/HelloImGui)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloAudio/CMakeLists.txt")
        add_subdirectory(samples/HelloAudio)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloNet/CMakeLists.txt")
        add_subdirectory(samples/HelloNet)
    endif()
endif()
