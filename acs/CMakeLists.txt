cmake_minimum_required(VERSION 3.24)
project(ACS LANGUAGES C CXX VERSION 0.1.0)

if(NOT WIN32)
    message(FATAL_ERROR "ACS currently targets Windows only")
endif()

# Visual Studio Solution Explorer をフォルダ分けする (engine / game / third_party 等)。
# 各 target に `FOLDER` プロパティを set すると、その階層に表示される。
# - acs_module()       → "engine"
# - acs_add_sample()   → "game"
# - acs_third_party_diligent() の Diligent ターゲット → "third_party"
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "Engine/Generated")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ACSCompilerOptions)
include(ACSModuleSystem)
include(ACSThirdParty)
include(ACSPackage)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# clang-tidy / IDE 言語サーバ (clangd, VS, Rider, CLion) のための compile_commands.json
# Phase 3c (StyleGuide v1) — リント基盤の最小実装。詳細は docs/StyleGuide.md §12.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "Export compile_commands.json for clang-tidy / IDE.")

# ---- UE5 風レイアウト: ソース/アセットだけが表層に見えるよう生成物を隔離 -----
# Binaries/    … 実行ファイル + 配布 DLL (TARGET_FILE_DIR もここ)
# Intermediate/… CMake cache / vcxproj / obj / .lib / FetchContent の _deps
# Saved/       … ログ・生成ファイル
# build tree (-B) は generate スクリプトが Intermediate/vs を指す。静的 lib と
# pdb は build tree に残し、表層には出さない。これらは generate.ps1 が hidden 属性
# を付与し .gitignore 済み。ACS_LAYOUT_ROOT で配置基準を上書きできる。
if(NOT DEFINED ACS_LAYOUT_ROOT OR "${ACS_LAYOUT_ROOT}" STREQUAL "")
    set(ACS_LAYOUT_ROOT "${CMAKE_SOURCE_DIR}")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${ACS_LAYOUT_ROOT}/Binaries")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${ACS_LAYOUT_ROOT}/Binaries")

option(ACS_BUILD_TESTS    "Build ACS unit tests"               ON)
option(ACS_BUILD_SAMPLES  "Build sample applications"          ON)
option(ACS_BUILD_TOOLS    "Build ACS CLI tools (acs_assetpack)" ON)
option(ACS_ENABLE_ASSERTS "Enable ACS_ASSERT in all builds"    ON)
set(ACS_STARTUP_PROJECT "hello_scene2d" CACHE STRING
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

# Phase 26+ 実バックエンド統合: Steamworks SDK の real 実装。
# OFF (default) なら src/gameframework/SteamworksBridge.h の Stub のみ利用可能。
# ON にする場合は cmake/ACSThirdParty.cmake の acs_third_party_steamworks() の
# コメントに従い、ACS_STEAMWORKS_SDK_DIR (推奨) か ACS_STEAMWORKS_SDK_URL を指定。
option(ACS_BUILD_STEAMWORKS "Build real Steamworks SDK backend (acs_steamworks module)" OFF)

# Phase N-3 実バックエンド統合: Lua 5.4 scripting の real 実装。
# OFF (default) なら src/gameframework/ScriptHost.h の ScriptVmStub のみ。
# ON にすると Lua 5.4 を FetchContent 取得 (MIT、gating 無し) し
# acs_scripting module (FLuaVm) を real ビルドする。
option(ACS_BUILD_SCRIPTING "Build real Lua 5.4 scripting backend (acs_scripting module)" OFF)

# Real ONNX Runtime CPU inference backend for acs::game::IMlRuntime.
# OFF (default) keeps only MlRuntimeStub. ON fetches/links the official
# ONNX Runtime Windows x64 package and builds ACS::MlOnnx.
option(ACS_BUILD_ML_ONNX "Build real ONNX Runtime backend (acs_mlonnx module)" OFF)

# Real Khronos OpenXR loader backend for acs::game::IOpenXrBridge.
# The backend is compiled and linked against OpenXR-SDK-Source. Runtime smoke
# tests accept XR_ERROR_RUNTIME_UNAVAILABLE as a valid "loader works, no XR
# runtime installed" result.
option(ACS_BUILD_OPENXR "Build real Khronos OpenXR backend (acs_openxr module)" OFF)

# Real Windows DbgHelp minidump backend for acs::game::ICrashReporterBackend.
# ON builds ACS::CrashWin and sample 43 can write a real .dmp file without
# external services or SDK accounts.
option(ACS_BUILD_CRASH_REPORTER "Build real Windows crash reporter backend (acs_crashwin module)" OFF)

# Real file-backed telemetry sink for acs::game::IBackendClient.
# ON builds ACS::TelemetryFile and sample 44 writes JSON Lines telemetry.
option(ACS_BUILD_TELEMETRY_FILE "Build real file telemetry backend (acs_telemetryfile module)" OFF)

# Real deterministic local matchmaker backend for acs::game::IMatchmaker.
# Useful for gameplay tests and offline development without a backend service.
option(ACS_BUILD_LOCAL_MATCHMAKER "Build real local matchmaker backend (acs_localmatch module)" OFF)

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
set_target_properties(acs_game PROPERTIES FOLDER "Game")
target_link_libraries(acs_game INTERFACE
    ACS::Foundation ACS::Threading ACS::Memory ACS::Container ACS::Math
    ACS::Platform ACS::Asset ACS::Ecs ACS::Render ACS::App)

if(ACS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- 5. CLI ツール群 (acs/tools/<name>/CMakeLists.txt) --------------------
# 配布用 ZIP には含めない開発時ユーティリティ。AssetPack モジュールが有効な
# ときだけ acs_assetpack を組み込む (依存欠落でビルドが落ちないように)。
if(ACS_BUILD_TOOLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/acs_assetpack/CMakeLists.txt")
    get_property(_acs_enabled_mods GLOBAL PROPERTY _ACS_ENABLED_MODULES)
    list(FIND _acs_enabled_mods "AssetPack" _acs_assetpack_idx)
    if(NOT _acs_assetpack_idx EQUAL -1)
        add_subdirectory(tools/acs_assetpack)
    endif()
endif()

# 「VS 2026 でこの 1 サンプルだけ開きたい」用フィルタ。空 = 全 sample 有効
# (デフォルト挙動)。scripts/gen-vs-solutions.ps1 からサンプルごとに値を渡す
# ことで、1 sln = engine + sample 1 個の小さなソリューションを生成できる。
set(ACS_ONLY_SAMPLE "" CACHE STRING
    "If set, only this sample dir is added (e.g. 28_HelloGameFramework). Empty = all.")

# `add_subdirectory(samples/<dir>)` のラッパ。ACS_ONLY_SAMPLE が空 or 一致時のみ追加。
# 追加後に取り込んだ全 target を Solution Explorer の "game" フォルダにまとめる。
function(acs_add_sample sample_dir)
    if("${ACS_ONLY_SAMPLE}" STREQUAL "" OR "${ACS_ONLY_SAMPLE}" STREQUAL "${sample_dir}")
        # add_subdirectory 前後の target diff を取って、サンプルで作られた target を game folder へ移す。
        get_property(_before_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        get_property(_before_imports DIRECTORY PROPERTY IMPORTED_TARGETS)
        set(_acs_prev_cmake_folder "${CMAKE_FOLDER}")
        set(CMAKE_FOLDER "Game")
        add_subdirectory(samples/${sample_dir})
        set(CMAKE_FOLDER "${_acs_prev_cmake_folder}")
        get_property(_after_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_t ${_after_targets})
            if(NOT _t IN_LIST _before_targets)
                # サンプルが作った target だけ FOLDER 設定 (INTERFACE は除外)
                get_target_property(_type ${_t} TYPE)
                if(NOT _type STREQUAL "INTERFACE_LIBRARY")
                    set_target_properties(${_t} PROPERTIES FOLDER "Game")
                endif()
            endif()
        endforeach()
    endif()
endfunction()

if(ACS_BUILD_SAMPLES)
    # サンプルは学習順に 00〜28 の番号付きディレクトリに並ぶ。
    # 一覧・推奨学習順・各サンプルのビルド要件は samples/README.md を参照。

    # --- (0) 最も簡単な入口: acs::easy レイヤ（クラス・継承なしで書ける）---
    acs_add_sample(00_HelloEasy)

    # --- (1) 入門: 2D とゲームの基礎（HLSL を書かない）---
    acs_add_sample(01_HelloWindow)
    acs_add_sample(02_HelloSprite)
    acs_add_sample(03_HelloText)
    acs_add_sample(04_HelloECS)
    acs_add_sample(05_HelloSave)
    acs_add_sample(06_HelloLocalization)
    acs_add_sample(07_HelloAudio)
    acs_add_sample(08_HelloPhysics2D)
    acs_add_sample(09_HelloParticles)

    # --- (2) 3D 入門: StandardShader（HLSL を書かない）---
    acs_add_sample(10_HelloModel)
    acs_add_sample(11_HelloRaycast3D)
    acs_add_sample(12_HelloLights)
    acs_add_sample(13_HelloSky)
    acs_add_sample(14_HelloShadows)
    acs_add_sample(15_HelloAnimation)

    # --- (3) 低レベル RHI: HLSL を直接書く発展編 ---
    acs_add_sample(16_HelloTriangle)
    acs_add_sample(17_HelloMesh)
    acs_add_sample(18_HelloTextured)

    # --- (4) UI・ツール・通信 ---
    acs_add_sample(19_HelloUI)
    # 20_HelloMVVM / 21_HelloImGui は acs_imgui が DX12 raw backend を直接叩く
    # 実装のため、Diligent 単独ビルドだと ImGui_ImplDX12_Init 等が未解決になる。
    # よって ACS_RENDER_DX12_RAW のときのみビルドする（両方有効ビルドは OK）。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(20_HelloMVVM)
        acs_add_sample(21_HelloImGui)
    endif()
    acs_add_sample(22_HelloNet)

    # --- (5) 上級グラフィックス ---
    acs_add_sample(23_HelloPbr)
    # 24〜27 は Diligent backend 専用（HDR / per-slice RT / cubemap など Diligent
    # 実装にのみ存在する機能を使う）。diligent-* プリセットでのみビルドされる。
    if(ACS_RENDER_DILIGENT)
        acs_add_sample(24_HelloBloom)
        acs_add_sample(25_HelloIbl)
        acs_add_sample(26_HelloLightmap)
        # 27 (Phase 35-3d 直後の cinematic demo) も Diligent 専用
        # (HDR + per-slice RT + RefractionShader が Diligent 要)
        acs_add_sample(27_HelloShowcase)
    endif()

    # --- (6) GameFramework (Phase 1 Pillar A: Scene 切替の最小デモ) ---
    acs_add_sample(28_HelloGameFramework)

    # --- (7) GameFramework Phase 19b: 対話的 Particle エディタ ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(29_HelloParticleEditor)
    endif()

    # --- (8) GameFramework Phase 20: SceneInspector (Hierarchy + Inspector + Toolbar) ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(30_HelloSceneInspector)
    endif()

    # --- (9) GameFramework Phase 21b: ModelViewer (editor_core + modelview/) ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(31_HelloModelViewer)
    endif()

    # --- (10) GameFramework Phase 22: AnimCurveEditor (editor_core + animcurve/) ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(32_HelloAnimCurveEditor)
    endif()

    # --- (11) GameFramework Phase 22: BehaviorTreeEditor (editor_core + btedit/) ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(33_HelloBehaviorTreeEditor)
    endif()

    # --- (12) GameFramework Phase 22: LevelEditor (editor_core + leveledit/) ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(34_HelloLevelEditor)
    endif()

    # --- (13) GameFramework Phase 22: SpriteAtlasEditor (editor_core + spriteatlas/) ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(35_HelloSpriteAtlasEditor)
    endif()

    # --- (14) GameFramework Phase 23: FontEditor (editor_core + fontedit/) ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(36_HelloFontEditor)
    endif()

    # --- (15) GameFramework Phase 23: CinematicsTimelineEditor (editor_core + cinetimeline/) ---
    # ImGui を使うため ACS_RENDER_DX12_RAW のみ (acs_imgui module の制約)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(37_HelloCinematicsEditor)
    endif()

    # --- (16) GameFramework Phase 25: HelloFullGame (Pillar A-X dogfood) ---
    # 完結 mini top-down shoot-em-up。5 wave クリアで Victory、HighScore 永続化。
    # ACS::Imgui に link するため ACS_RENDER_DX12_RAW のみ (sample 本体は ImGui
    # を直接使わないが、CMakeLists で link 指定があるためガード必要)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(38_HelloFullGame)
    endif()

    # --- (17) Phase 26: HelloSteamworks (ISteamworksBridge デモ) ---
    # stub mode (default) でビルド可能。ACS_BUILD_STEAMWORKS=ON で real backend。
    # DX12 raw のみ (SpriteBatch/Font HUD のため、他サンプルと同条件)。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(39_HelloSteamworks)
    endif()

    # --- (18) Phase N-3: HelloScripting (IScriptVm / Lua 5.4 デモ) ---
    # window / renderer 不要のコンソールアプリ。stub mode (default) でビルド可能。
    # ACS_BUILD_SCRIPTING=ON で real Lua 5.4 backend (ACS::Scripting) をリンク。
    acs_add_sample(40_HelloScripting)

    # --- (19) HelloOnnx (IMlRuntime / ONNX Runtime real backend) ---
    # Real backend smoke test. Only added when ACS::MlOnnx is enabled.
    if(ACS_BUILD_ML_ONNX)
        acs_add_sample(41_HelloOnnx)
    endif()

    # --- (20) HelloOpenXR (IOpenXrBridge / Khronos OpenXR loader) ---
    # Real loader smoke test. Accepts "runtime unavailable" as a valid result
    # because CI/dev machines often have no active OpenXR runtime installed.
    if(ACS_BUILD_OPENXR)
        acs_add_sample(42_HelloOpenXR)
    endif()

    # --- (21) HelloCrashReporter (ICrashReporterBackend / Windows minidump) ---
    # Real DbgHelp smoke test. Writes a .txt report and .dmp minidump.
    if(ACS_BUILD_CRASH_REPORTER)
        acs_add_sample(43_HelloCrashReporter)
    endif()

    # --- (22) HelloTelemetryFile (IBackendClient / JSONL telemetry sink) ---
    # Real local telemetry sink. Writes events through FTelemetryDirector.
    if(ACS_BUILD_TELEMETRY_FILE)
        acs_add_sample(44_HelloTelemetryFile)
    endif()

    # --- (23) HelloLocalMatchmaker (IMatchmaker / deterministic local queue) ---
    # Real offline matchmaker. Exercises search -> matched -> accept/cancel.
    if(ACS_BUILD_LOCAL_MATCHMAKER)
        acs_add_sample(45_HelloLocalMatchmaker)
    endif()

    # --- (24) HelloAssetPackBridge (IAssetPackReader/Writer / .acpak) ---
    # Real adapter smoke test. Writes a pack, mounts it, enumerates and reads bytes.
    acs_add_sample(46_HelloAssetPackBridge)

    # --- (25) HelloLight2D (2D 動的ライト + ソフト影 + ブロブ影、Core Keeper 風) ---
    # ACS::Render の FLighting2D / FBlobShadow を使う 2D サンプル。SpriteBatch +
    # offscreen RT を使うため DX12 raw のみ。
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(47_HelloLight2D)
    endif()

    # --- (26) HelloSpriteCollider / HelloMeshCollider (ACS::Collision) ---
    # スプライト alpha → 形状コライダー / 3D メッシュ → 三角形 BVH コライダーの
    # 検証コンソール。GPU 不要・ヘッドレスで完結 (assert ベース)。
    acs_add_sample(48_HelloSpriteCollider)
    acs_add_sample(49_HelloMeshCollider)

    # --- (27) HelloSpritePhysics (凸包コライダー → FCollisionWorld2D 統合) ---
    acs_add_sample(50_HelloSpritePhysics)

    # --- (28) HelloConvexHull (3D 凸包生成 → 凸コライダー) ---
    acs_add_sample(51_HelloConvexHull)

    # --- (29) HelloColliderViz2D (スプライトコライダーの目視確認、windowed) ---
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(52_HelloColliderViz2D)
    endif()

    # --- (30) HelloColliderViz3D (3D メッシュ/凸包の wireframe 目視確認、windowed) ---
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(53_HelloColliderViz3D)
    endif()

    # --- (31) HelloCollideSlide (FPhysicsBody2D collide-and-slide 検証、console) ---
    acs_add_sample(54_HelloCollideSlide)

    # --- (32) HelloScene2D (FScene2D starter foundation, windowed) ---
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(55_HelloScene2D)
    endif()

    # --- (33) HelloSpriteAnim (Package A: sprite-sheet anim + HUD text, windowed) ---
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(56_HelloSpriteAnim)
    endif()

    # --- (34) HelloTriggers (Package B: collision layers/masks + triggers, console) ---
    acs_add_sample(57_HelloTriggers)

    # --- (35) HelloTilemap (Package C: tilemap render + fade scene transition, windowed) ---
    if(ACS_RENDER_DX12_RAW)
        acs_add_sample(58_HelloTilemap)
    endif()
endif()

if(ACS_STARTUP_PROJECT AND TARGET ${ACS_STARTUP_PROJECT})
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY
        VS_STARTUP_PROJECT ${ACS_STARTUP_PROJECT})
elseif(TARGET hello_easy)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY
        VS_STARTUP_PROJECT hello_easy)
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
    set_target_properties(acs_lint PROPERTIES FOLDER "Engine/Tools")
    message(STATUS "ACS: acs_lint ターゲットを登録 (clang-tidy at ${ACS_CLANG_TIDY_EXE})")
else()
    message(STATUS "ACS: clang-tidy が見つからないため acs_lint ターゲットは未登録。")
    message(STATUS "       インストール後 cmake を再 configure すれば有効になる。")
endif()

# ============================================================================
# 配布パッケージング (Phase 4 beginner-UX)
# ----------------------------------------------------------------------------
# acs_package_game() を呼んだ実行ファイル (samples/00_HelloEasy 等) を
# `cmake --build <build> --target package` で ZIP 化する。詳細は
# cmake/ACSPackage.cmake のヘッダコメント。
# ============================================================================
acs_enable_packaging()
