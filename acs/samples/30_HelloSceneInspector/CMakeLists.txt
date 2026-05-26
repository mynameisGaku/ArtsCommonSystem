# SPDX-License-Identifier: Apache-2.0
# HelloSceneInspector — SceneInspector デモ。
# Node2D ツリー (root → wheel → spoke[0/1] など複数階層) を `HierarchyPanel`
# で可視化し、`InspectorPanel` が `IInspectableProvider` 経由で公開された
# field を ImGui で編集する単一ウィンドウのエディタサンプル。`EditorToolbar`
# で Play/Pause/Step などのコマンドを発行、`SelectionService` で 4 パネル間の
# 選択を共有。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor と同じ。
#
# 構成:
#   main.cpp                    - ACS_GAME_MAIN(SceneInspectorApp) のみ
#   SceneNodes.{h,cpp}          - PlayerNode + WheelNode
#   PanelLayout.{h,cpp}         - 4 panel + Selection + Seam の束ね
#   SceneInspectorScene.{h,cpp} - Node2D ツリー + PanelLayout を持つ Scene
#   SceneInspectorApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
add_executable(hello_scene_inspector WIN32
    main.cpp
    SceneNodes.cpp
    PanelLayout.cpp
    SceneInspectorScene.cpp
    SceneInspectorApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    SceneNodes.h
    PanelLayout.h
    SceneInspectorScene.h
    SceneInspectorApp.h
)
acs_apply_compiler_options(hello_scene_inspector)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "SceneInspectorApp.h" のように local include で書けるようにする)
target_include_directories(hello_scene_inspector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_scene_inspector PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
    ACS::Imgui   # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
