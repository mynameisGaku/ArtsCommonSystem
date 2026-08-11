cmake_minimum_required(VERSION 3.24)
project(ACS LANGUAGES C CXX VERSION 0.1.0)

if(NOT WIN32)
    message(FATAL_ERROR "ACS currently targets Windows only")
endif()

# Visual Studio Solution Explorer をフォルダ分けする (engine / third_party 等)。
# 各 target に `FOLDER` プロパティを set すると、その階層に表示される。
# - acs_module()       → "engine"
# - acs_third_party_diligent() の Diligent ターゲット → "third_party"
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Engine/Generated")

# build system は acs/engine/、source/asset tree (src, tests, tools) は
# 1 階層上の acs/ に置く。ACS_TREE_ROOT はその acs/ directory を指し、以下の
# tree 相対 path はこの list directory ではなく ACS_TREE_ROOT を基準にする。
get_filename_component(ACS_TREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
set(ACS_TREE_ROOT "${ACS_TREE_ROOT}" CACHE INTERNAL "ACS source-tree root (parent of engine/)")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ACSCompilerOptions)
include(ACSModuleSystem)
include(ACSThirdParty)
include(ACSPackage)
include(CheckIPOSupported)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC の既定 /EHsc は ACS の例外無効化指定 /EHs-c- と競合し、全翻訳単位で
# D9025 を発生させる。ACS の親スコープからだけ除去し、例外を使うサードパーティは
# 各 FetchContent スコープで必要な指定を明示する。
if(MSVC)
    string(REGEX REPLACE "(^|[ \t])[/\\-]EHsc([ \t]|$)" " "
           CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(STRIP "${CMAKE_CXX_FLAGS}" CMAKE_CXX_FLAGS)
endif()

# clang-tidy / IDE 言語サーバ (clangd, VS, Rider, CLion) のための compile_commands.json
# StyleGuide v1 に基づくリント基盤。詳細は docs/StyleGuide.md §12.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "Export compile_commands.json for clang-tidy / IDE.")

# ---- UE5 風レイアウト: ソース/アセットだけが表層に見えるよう生成物を隔離 -----
# Binaries/    … 実行ファイル + 配布 DLL (TARGET_FILE_DIR もここ)
# Intermediate/… CMake cache / vcxproj / obj / .lib / FetchContent の _deps
# Saved/       … ログ・生成ファイル
# build tree (-B) は generate スクリプトが Intermediate/vs を指す。静的 lib と
# pdb は build tree に残し、表層には出さない。これらは generate.ps1 が hidden 属性
# を付与し .gitignore 済み。ACS_LAYOUT_ROOT で配置基準を上書きできる。
if(NOT DEFINED ACS_LAYOUT_ROOT OR "${ACS_LAYOUT_ROOT}" STREQUAL "")
    set(ACS_LAYOUT_ROOT "${ACS_TREE_ROOT}")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${ACS_LAYOUT_ROOT}/Binaries")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${ACS_LAYOUT_ROOT}/Binaries")

option(ACS_BUILD_TESTS    "Build ACS unit tests"               ON)
option(ACS_BUILD_TOOLS    "Build ACS CLI tools (acs_assetpack)" ON)
option(ACS_ENABLE_ASSERTS "Enable ACS_ASSERT in all builds"    ON)
option(ACS_ENABLE_WATER_TAA_EVIDENCE
       "Build the opt-in deterministic water TAA evidence fixture" OFF)
option(ACS_ENABLE_ADDRESS_SANITIZER
       "Instrument ACS targets with AddressSanitizer"          OFF)
option(ACS_ENABLE_RELEASE_IPO
       "Enable interprocedural optimization for Release ACS targets" ON)
option(ACS_ENABLE_DISTRIBUTION_CONSUMER_SMOKE
       "Link and run the generated Debug/Release distribution SDK" OFF)
set(ACS_DISTRIBUTION_CONSUMER_ROOT "" CACHE PATH
    "Distribution SDK root used by the Debug/Release consumer smoke")
set(ACS_COMPILED_LOG_MIN_SEVERITY "0" CACHE STRING
    "Compile-time log minimum: 0=Trace, 1=Debug, 2=Info, 3=Warn, 4=Error, 5=Fatal, 6=Off")
if(NOT ACS_COMPILED_LOG_MIN_SEVERITY MATCHES "^[0-6]$")
    message(FATAL_ERROR
        "ACS_COMPILED_LOG_MIN_SEVERITY must be one digit from 0 through 6")
endif()

# Release の実行速度を優先して IPO/LTCG を既定有効にする。ASan とは計装目的が異なり
# リンク互換性も保証できないため併用せず、ツールチェーン非対応時も安全に無効化する。
set(ACS_RELEASE_IPO_ACTIVE OFF)
if(ACS_ENABLE_RELEASE_IPO AND NOT ACS_ENABLE_ADDRESS_SANITIZER)
    check_ipo_supported(RESULT ACS_RELEASE_IPO_ACTIVE
                        OUTPUT ACS_RELEASE_IPO_DIAGNOSTIC
                        LANGUAGES CXX)
    if(NOT ACS_RELEASE_IPO_ACTIVE)
        message(WARNING
            "ACS: Release IPO/LTCG is unavailable: ${ACS_RELEASE_IPO_DIAGNOSTIC}")
    endif()
endif()

# MSVC の Debug 既定値に含まれる /RTC は AddressSanitizer と併用できない。
# 個々のターゲットではなくキャッシュ変数から除去し、依存ターゲットにも残さない。
if(MSVC AND ACS_ENABLE_ADDRESS_SANITIZER)
    foreach(_acs_flag_variable CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
        string(REGEX REPLACE "(^| )[/-]RTC[^ ]*" "" ${_acs_flag_variable}
               "${${_acs_flag_variable}}")
    endforeach()
    # FetchContent 配下を含む全ターゲットを同一条件で計装する。境界をまたぐ
    # 解放後使用を検出するには、ACS本体だけの部分的な計装では不十分なため。
    add_compile_options(/fsanitize=address /Oy- /Zi)
    add_link_options(/INCREMENTAL:NO /DEBUG:FULL)
endif()
set(ACS_STARTUP_PROJECT "" CACHE STRING
    "Visual Studio startup project when the single ACS solution is opened.")

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

# Steamworks SDK を使う実バックエンド。
# OFF (default) なら src/gameframework/SteamworksBridge.h の Stub のみ利用可能。
# ONではacs_third_party_steamworks()がDIR、URL、Gitの順でSDK入力を解決する。
option(ACS_BUILD_STEAMWORKS "Build real Steamworks SDK backend (acs_steamworks module)" OFF)

# Lua 5.4 を使う scripting 実バックエンド。
# OFF (default) なら src/gameframework/ScriptHost.h の ScriptVmStub のみ。
# ON にすると Lua 5.4 を FetchContent 取得 (MIT、gating 無し) し
# acs_scripting module (CLuaVm) を real ビルドする。
option(ACS_BUILD_SCRIPTING "Build real Lua 5.4 scripting backend (acs_scripting module)" OFF)

# acs::game::IMlRuntime 用の実 ONNX Runtime CPU inference backend。
# OFF (既定) は MlRuntimeStub のみを保ち、ON は公式 ONNX Runtime Windows x64
# package を取得・link して ACS::MlOnnx を構築する。
option(ACS_BUILD_ML_ONNX "Build real ONNX Runtime backend (acs_mlonnx module)" OFF)

# acs::game::IOpenXrBridge 用の実 Khronos OpenXR loader backend。
# OpenXR-SDK-Source に対して compile/link する。runtime smoke test は
# XR_ERROR_RUNTIME_UNAVAILABLE を「loader は動作したが XR runtime がない」
# という有効な結果として扱う。
option(ACS_BUILD_OPENXR "Build real Khronos OpenXR backend (acs_openxr module)" OFF)

# acs::game::ICrashReporterBackend 用の実 Windows DbgHelp minidump backend。
# ON は ACS::CrashWin を構築し、外部 service/SDK account なしで実 .dmp file を書く。
option(ACS_BUILD_CRASH_REPORTER "Build real Windows crash reporter backend (acs_crashwin module)" OFF)

# acs::game::IBackendClient 用の実 file-backed telemetry sink。
# ON は ACS::TelemetryFile を構築し、JSON Lines telemetry を書く。
option(ACS_BUILD_TELEMETRY_FILE "Build real file telemetry backend (acs_telemetryfile module)" OFF)

# acs::game::IMatchmaker 用の実 deterministic local matchmaker backend。
# 外部serviceなしで同じ入力から同じmatching結果を返す。
option(ACS_BUILD_LOCAL_MATCHMAKER "Build real local matchmaker backend (acs_localmatch module)" OFF)

set(ACS_SOURCE_ROOT "${ACS_TREE_ROOT}/src")

# ---- 1. src/<mod>/Module.cmake から利用可能なモジュールを発見する ---------
acs_discover_modules("${ACS_SOURCE_ROOT}")

# ---- 2. modules.cmake が有効moduleを選択する -------------------------------
include("${CMAKE_CURRENT_SOURCE_DIR}/modules.cmake")

# ---- 3. 依存関係を検証して target を生成、WITH_ACS_* define を付与 -------
acs_finalize_modules()

# ---- 4. ゲーム向けの集約ターゲット ACS::Game ------------------------------
# 標準的なゲームに必要な 10 モジュールを 1 つにまとめた INTERFACE ターゲット。
# 音声、network、ImGui、MVVM、UIは用途ごとの追加targetとして分離する。
add_library(acs_game INTERFACE)
add_library(ACS::Game ALIAS acs_game)
set_target_properties(acs_game PROPERTIES FOLDER "Game")
target_link_libraries(acs_game INTERFACE
    ACS::Foundation ACS::Threading ACS::Memory ACS::Container ACS::Math
    ACS::Platform ACS::Asset ACS::Ecs ACS::Render ACS::App)

# ---- 4b. ACS Editor ABI: C# (WPF) エディタが P/Invoke する C ABI DLL ---------
# 外部 HWND にエンジンの DX12 スワップチェインをホストして描画するブリッジ。
# DX12 raw backend 専用。出力は Binaries/<config>/acs_editor_abi.dll。
if(ACS_RENDER_DX12_RAW AND EXISTS "${ACS_SOURCE_ROOT}/editor_abi/EditorAbi.cpp")
    add_library(acs_editor_abi SHARED "${ACS_SOURCE_ROOT}/editor_abi/EditorAbi.cpp")
    add_library(ACS::EditorAbi ALIAS acs_editor_abi)
    acs_apply_compiler_options(acs_editor_abi)   # /utf-8 + C++ 標準 + 警告レベル (engine と同条件)
    # ACS::Game (集約) に加え GameFramework をリンク: リフレクションレジストリ
    # (FTypeRegistry) と ReflectCatalog をエディタ ABI から使えるようにする。
    target_link_libraries(
        acs_editor_abi PRIVATE
        ACS::Game ACS::GameFramework ACS::Collision)
    target_include_directories(acs_editor_abi PRIVATE "${ACS_SOURCE_ROOT}")
    set_target_properties(acs_editor_abi PROPERTIES FOLDER "Game")
endif()

# ---- 4c. 外部projectの取り込み ---------------------------------------------
# ACS_EXTERNAL_PROJECT_DIRのCMakeListsを追加し、既存engine libraryへlinkするtargetを構築する。
if(ACS_EXTERNAL_PROJECT_DIR AND EXISTS "${ACS_EXTERNAL_PROJECT_DIR}/CMakeLists.txt")
    add_subdirectory("${ACS_EXTERNAL_PROJECT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_extproj")
    message(STATUS "ACS: 外部プロジェクトを取り込み: ${ACS_EXTERNAL_PROJECT_DIR}")
endif()

# ---- acsbuild: .Build.cs ↔ Module.cmake のドリフト検証ターゲット (dev 用・任意) -----
# 各src/<mod>/<Name>.Build.csの生成内容とtracked Module.cmakeのdriftを検出する。
# ALL_BUILD には含めない明示ターゲット。dotnet がある環境だけ登録する。
find_program(ACS_DOTNET_EXE dotnet)
if(ACS_DOTNET_EXE)
    add_custom_target(acs_buildcs_check
        COMMAND "${ACS_DOTNET_EXE}" run --project "${ACS_TREE_ROOT}/tools/acsbuild/AcsBuild.csproj"
                -c Release -- --check --root "${ACS_TREE_ROOT}"
        WORKING_DIRECTORY "${ACS_TREE_ROOT}"
        COMMENT "acsbuild: .Build.cs と Module.cmake の整合を検証 (--check)"
        USES_TERMINAL VERBATIM)
    set_target_properties(acs_buildcs_check PROPERTIES FOLDER "Tools")
else()
    message(STATUS "ACS: dotnet 未検出のため acs_buildcs_check ターゲットを skip")
endif()

if(ACS_BUILD_TESTS)
    enable_testing()
    add_subdirectory("${ACS_TREE_ROOT}/tests" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()

# ---- 5. CLI ツール群 (acs/tools/<name>/CMakeLists.txt) --------------------
# 配布用 ZIP には含めない開発時ユーティリティ。AssetPack モジュールが有効な
# ときだけ acs_assetpack を組み込む (依存欠落でビルドが落ちないように)。
if(ACS_BUILD_TOOLS AND EXISTS "${ACS_TREE_ROOT}/tools/acs_assetpack/CMakeLists.txt")
    get_property(_acs_enabled_mods GLOBAL PROPERTY _ACS_ENABLED_MODULES)
    list(FIND _acs_enabled_mods "AssetPack" _acs_assetpack_idx)
    if(NOT _acs_assetpack_idx EQUAL -1)
        add_subdirectory("${ACS_TREE_ROOT}/tools/acs_assetpack" "${CMAKE_CURRENT_BINARY_DIR}/tools/acs_assetpack")
    endif()
endif()

if(ACS_STARTUP_PROJECT AND TARGET ${ACS_STARTUP_PROJECT})
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY
        VS_STARTUP_PROJECT ${ACS_STARTUP_PROJECT})
endif()

# acs_lintは.clang-tidyとcompile_commands.jsonを使い、src/testsを明示targetで走査する。
# WarningsAsErrorsの対象は.clang-tidyが所有し、未検出clang-tidyではtargetを登録しない。
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
        "${ACS_TREE_ROOT}/src/*.h"
        "${ACS_TREE_ROOT}/src/*.hpp"
        "${ACS_TREE_ROOT}/src/*.cpp"
        "${ACS_TREE_ROOT}/src/*.inl"
        "${ACS_TREE_ROOT}/tests/*.cpp")
    # _deps/ や cmake-build-* 以下は除外（third-party / build artifact）
    list(FILTER ACS_LINT_SOURCES EXCLUDE REGEX "(/_deps/|/cmake-build-)")

    add_custom_target(acs_lint
        COMMAND ${ACS_CLANG_TIDY_EXE}
                --quiet
                -p "${CMAKE_BINARY_DIR}"
                --config-file=${ACS_TREE_ROOT}/../.clang-tidy
                ${ACS_LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy on src/ tests/ (config: .clang-tidy)"
        VERBATIM USES_TERMINAL)
    set_target_properties(acs_lint PROPERTIES FOLDER "Engine/Tools")
    message(STATUS "ACS: acs_lint ターゲットを登録 (clang-tidy at ${ACS_CLANG_TIDY_EXE})")
else()
    message(STATUS "ACS: clang-tidy が見つからないため acs_lint ターゲットは未登録。")
    message(STATUS "       インストール後 cmake を再 configure すれば有効になる。")
endif()

# acs_enable_packagingはacs_package_game登録targetとruntime payloadをZIPへまとめる。
acs_enable_packaging()
