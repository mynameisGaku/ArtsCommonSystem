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

# clang-tidy / IDE 言語サーバ (clangd, VS, Rider, CLion) のための compile_commands.json
# Phase 3c (StyleGuide v1) — リント基盤の最小実装。詳細は docs/StyleGuide.md §12.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "Export compile_commands.json for clang-tidy / IDE.")

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

# MVVM の ImGui アダプタをビルドするか。OFF にすれば Mvvm モジュールは Imgui に依存しない。
option(ACS_MVVM_IMGUI_BINDINGS "Build MVVM ImGui binding adapter (requires Imgui module)" ON)

set(ACS_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---- 1. src/<mod>/Module.cmake から利用可能なモジュールを発見する ---------
acs_discover_modules("${ACS_SOURCE_ROOT}")

# ---- 2. ユーザーが modules.cmake で有効化する -----------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/modules.cmake")

# ---- 3. 依存関係を検証して target を生成、WITH_ACS_* define を付与 -------
acs_finalize_modules()

# ---- 4. サンプル / 自作ゲーム向けの集約ターゲット ACS::Game ----------------
# 標準的なゲームに必要な 10 モジュールを 1 つにまとめた INTERFACE ターゲット。
# サンプルや自作ゲームの CMakeLists は target_link_libraries(mygame PRIVATE
# ACS::Game) の 1 行で済む。音声 / ネットワーク / ImGui / MVVM / UI などの
# 追加モジュールは ACS::Audio などを個別に足す。
add_library(acs_game INTERFACE)
add_library(ACS::Game ALIAS acs_game)
target_link_libraries(acs_game INTERFACE
    ACS::Foundation ACS::Threading ACS::Memory ACS::Container ACS::Math
    ACS::Platform ACS::Asset ACS::Ecs ACS::Render ACS::App)

if(ACS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ACS_BUILD_SAMPLES)
    # サンプルは学習順に 00〜26 の番号付きディレクトリに並ぶ。
    # 一覧・推奨学習順・各サンプルのビルド要件は samples/README.md を参照。

    # --- (0) 最も簡単な入口: acs::easy レイヤ（クラス・継承なしで書ける）---
    add_subdirectory(samples/00_HelloEasy)

    # --- (1) 入門: 2D とゲームの基礎（HLSL を書かない）---
    add_subdirectory(samples/01_HelloWindow)
    add_subdirectory(samples/02_HelloSprite)
    add_subdirectory(samples/03_HelloText)
    add_subdirectory(samples/04_HelloECS)
    add_subdirectory(samples/05_HelloSave)
    add_subdirectory(samples/06_HelloLocalization)
    add_subdirectory(samples/07_HelloAudio)
    add_subdirectory(samples/08_HelloPhysics2D)
    add_subdirectory(samples/09_HelloParticles)

    # --- (2) 3D 入門: StandardShader（HLSL を書かない）---
    add_subdirectory(samples/10_HelloModel)
    add_subdirectory(samples/11_HelloRaycast3D)
    add_subdirectory(samples/12_HelloLights)
    add_subdirectory(samples/13_HelloSky)
    add_subdirectory(samples/14_HelloShadows)
    add_subdirectory(samples/15_HelloAnimation)

    # --- (3) 低レベル RHI: HLSL を直接書く発展編 ---
    add_subdirectory(samples/16_HelloTriangle)
    add_subdirectory(samples/17_HelloMesh)
    add_subdirectory(samples/18_HelloTextured)

    # --- (4) UI・ツール・通信 ---
    add_subdirectory(samples/19_HelloUI)
    # 20_HelloMVVM / 21_HelloImGui は acs_imgui が DX12 raw backend を直接叩く
    # 実装のため、Diligent 単独ビルドだと ImGui_ImplDX12_Init 等が未解決になる。
    # よって ACS_RENDER_DX12_RAW のときのみビルドする（両方有効ビルドは OK）。
    if(ACS_RENDER_DX12_RAW)
        add_subdirectory(samples/20_HelloMVVM)
        add_subdirectory(samples/21_HelloImGui)
    endif()
    add_subdirectory(samples/22_HelloNet)

    # --- (5) 上級グラフィックス ---
    add_subdirectory(samples/23_HelloPbr)
    # 24〜26 は Diligent backend 専用（HDR / per-slice RT / cubemap など Diligent
    # 実装にのみ存在する機能を使う）。diligent-* プリセットでのみビルドされる。
    if(ACS_RENDER_DILIGENT)
        add_subdirectory(samples/24_HelloBloom)
        add_subdirectory(samples/25_HelloIbl)
        add_subdirectory(samples/26_HelloLightmap)
        # 27 (Phase 35-3d 直後の cinematic demo) も Diligent 専用
        # (HDR + per-slice RT + RefractionShader が Diligent 要)
        add_subdirectory(samples/27_HelloShowcase)
    endif()
endif()

# ============================================================================
# acs_lint — clang-tidy リントターゲット (Phase 3c)
# ----------------------------------------------------------------------------
# 使い方:  cmake --build <build-dir> --target acs_lint
# 役割:    .clang-tidy + compile_commands.json で src/ samples/ tests/ を走査
# 範囲:    PR で変更されたファイルのみリントしたい場合は run-clang-tidy.py に
#          -files <regex> を渡して呼び出す。WarningsAsErrors は .clang-tidy 側
#          で narrow に絞ってあるため、初回でも CI ゲートが破綻しない。
# 詳細:    docs/StyleGuide.md §12.6
# ============================================================================
find_program(ACS_CLANG_TIDY_EXE
    NAMES clang-tidy clang-tidy-18 clang-tidy-17 clang-tidy-16
    HINTS "$ENV{ProgramFiles}/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin"
          "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
          "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/x64/bin"
          "$ENV{ProgramFiles}/LLVM/bin"
          "C:/Program Files/LLVM/bin"
    DOC "clang-tidy executable used by the acs_lint target")

if(ACS_CLANG_TIDY_EXE)
    file(GLOB_RECURSE ACS_LINT_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.inl"
        "${CMAKE_CURRENT_SOURCE_DIR}/samples/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    # _deps/ や cmake-build-* 以下は除外（third-party / build artifact）
    list(FILTER ACS_LINT_SOURCES EXCLUDE REGEX "(/_deps/|/cmake-build-)")

    add_custom_target(acs_lint
        COMMAND ${ACS_CLANG_TIDY_EXE}
                --quiet
                -p "${CMAKE_BINARY_DIR}"
                --config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy
                ${ACS_LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on src/ samples/ tests/ (config: .clang-tidy)"
        VERBATIM USES_TERMINAL)
    message(STATUS "ACS: acs_lint ターゲットを登録 (clang-tidy at ${ACS_CLANG_TIDY_EXE})")
else()
    message(STATUS "ACS: clang-tidy が見つからないため acs_lint ターゲットは未登録。")
    message(STATUS "       インストール後 cmake を再 configure すれば有効になる。")
endif()
