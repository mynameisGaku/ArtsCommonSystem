cmake_minimum_required(VERSION 3.24)
project(ACS LANGUAGES C CXX VERSION 0.1.0)

if(NOT WIN32)
    message(FATAL_ERROR "ACS currently targets Windows only")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ACSCompilerOptions)
include(ACSModuleSystem)
include(ACSThirdParty)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACS_BUILD_TESTS    "Build ACS unit tests"               ON)
option(ACS_BUILD_SAMPLES  "Build sample applications"          ON)
option(ACS_ENABLE_ASSERTS "Enable ACS_ASSERT in all builds"    ON)

# RHI バックエンド選択（Render モジュールが参照する。ACS_Render_* 系の feature と
# 一致させ、Module.cmake のソース/リンク条件にも使う）。
# ACS_RENDER_DX12_RAW : 既存の独自 DX12 バックエンド（自前実装）
# ACS_RENDER_DILIGENT : Diligent Engine 経由のクロスプラットフォーム RHI
# 両方 ON でも構わない（実行時に CreateRhiDevice が振り分ける）。
option(ACS_RENDER_DX12_RAW "Build raw DX12 RHI backend"            ON)
option(ACS_RENDER_DILIGENT "Build Diligent Engine RHI backend"     OFF)
option(ACS_DILIGENT_VULKAN "Build Diligent Vulkan backend (requires ACS_RENDER_DILIGENT=ON)" OFF)

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
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloMesh/CMakeLists.txt")
        add_subdirectory(samples/HelloMesh)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloTextured/CMakeLists.txt")
        add_subdirectory(samples/HelloTextured)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloModel/CMakeLists.txt")
        add_subdirectory(samples/HelloModel)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloSprite/CMakeLists.txt")
        add_subdirectory(samples/HelloSprite)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloText/CMakeLists.txt")
        add_subdirectory(samples/HelloText)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloPhysics2D/CMakeLists.txt")
        add_subdirectory(samples/HelloPhysics2D)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloRaycast3D/CMakeLists.txt")
        add_subdirectory(samples/HelloRaycast3D)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloSky/CMakeLists.txt")
        add_subdirectory(samples/HelloSky)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloSave/CMakeLists.txt")
        add_subdirectory(samples/HelloSave)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloParticles/CMakeLists.txt")
        add_subdirectory(samples/HelloParticles)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloAnimation/CMakeLists.txt")
        add_subdirectory(samples/HelloAnimation)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloLocalization/CMakeLists.txt")
        add_subdirectory(samples/HelloLocalization)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloLights/CMakeLists.txt")
        add_subdirectory(samples/HelloLights)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloShadows/CMakeLists.txt")
        add_subdirectory(samples/HelloShadows)
    endif()
    # HelloBloom は Diligent backend 専用のため、ACS_RENDER_DILIGENT のときだけ追加
    if(ACS_RENDER_DILIGENT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloBloom/CMakeLists.txt")
        add_subdirectory(samples/HelloBloom)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples/HelloMVVM/CMakeLists.txt")
        add_subdirectory(samples/HelloMVVM)
    endif()
endif()
