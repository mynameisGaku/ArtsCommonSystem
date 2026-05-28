# SPDX-License-Identifier: Apache-2.0
# HelloLevelEditor — Tilemap (Level) エディタ。
# editor_core (EditorWorkspace + EditorTheme) と leveledit/LevelEditorPanel と
# Tilemap (Pillar Q) を 1 個の ImGui Workspace に統合した level editor デモ。
#
# 構成:
#   main.cpp                - ACS_GAME_MAIN(LevelEditorApp) のみ
#   LevelEditorApp.{h,cpp}  - Game 派生クラス (ImGui lifecycle ラッパ)
#   LevelEditorScene.{h,cpp}- Workspace + LevelEditorPanel + Tilemap を持つ Scene
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui などと同じ。
add_executable(hello_level_editor WIN32
    main.cpp
    LevelEditorApp.cpp
    LevelEditorScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    LevelEditorApp.h
    LevelEditorScene.h
)
acs_apply_compiler_options(hello_level_editor)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "LevelEditorApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_level_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_level_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui      # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
    ACS::Math
)
