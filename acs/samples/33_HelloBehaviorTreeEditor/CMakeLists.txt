# HelloBehaviorTreeEditor — BehaviorTree Editor デモ
#
# `gameframework/BehaviorTree.h` (Pillar L) で組んだ小さな BT を、
# `btedit::BehaviorTreeEditorPanel` で **可視化 + step debug** する単一
# ウィンドウのエディタサンプル。Action ノードが panel 側の SetNodeStatus を
# 呼んで status を push する典型用途を見せる。
#
# 構成:
#   main.cpp              - ACS_GAME_MAIN(BtEditorApp) のみ
#   BtEditorApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
#   BtEditorScene.{h,cpp} - Scene lifecycle (OnEnter/OnExit/OnUpdate/OnRender)
#   TreeActions.{h,cpp}   - Action Fn 群 + step callback
#   TreeBuilder.{h,cpp}   - panel メタミラー組立 + 実 BT 構築 helper
#
# btedit/ panel の .cpp はサンプル側でビルド (Module.cmake には未追加)。
# include path は `ACS::GameFramework` が PUBLIC で ${ACS_SOURCE_ROOT} を
# 公開しているため `#include "gameframework/tools/btedit/..."` がそのまま通る。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# / 31_HelloModelViewer と同じ。
add_executable(hello_behavior_tree_editor WIN32
    main.cpp
    BtEditorApp.cpp
    BtEditorScene.cpp
    TreeActions.cpp
    TreeBuilder.cpp
    # btedit panel 本体 (sample 側でビルドさせる、Module.cmake 不変)。
    ${CMAKE_SOURCE_DIR}/src/gameframework/tools/btedit/BehaviorTreeEditorPanel.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    BtEditorApp.h
    BtEditorScene.h
    TreeActions.h
    TreeBuilder.h
)
acs_apply_compiler_options(hello_behavior_tree_editor)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "BtEditorApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_behavior_tree_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_behavior_tree_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
    ACS::Imgui   # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
)
