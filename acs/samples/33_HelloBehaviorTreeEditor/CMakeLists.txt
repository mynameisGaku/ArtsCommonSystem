# HelloBehaviorTreeEditor — GameFramework Phase 22: BehaviorTree Editor デモ
#
# `gameframework/BehaviorTree.h` (Pillar L) で組んだ小さな BT を、
# `btedit::BehaviorTreeEditorPanel` (= Phase 22 editor 第三弾) で **可視化 +
# step debug** する単一ウィンドウのエディタサンプル。Action ノードが panel
# 側の SetNodeStatus を呼んで status を push する典型用途を見せる。
#
# 構成:
#   main.cpp              - ACS_GAME_MAIN(BtEditorApp) のみ
#   BtEditorApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
#   BtEditorScene.{h,cpp} - BT 構築 + Panel + Action Fn 群 + step callback
#
# btedit/ panel の .cpp はサンプル側でビルド (Module.cmake には未追加。
# Phase 23 で Workspace 統合と一緒に Module.cmake へ昇格させる予定)。
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
    # btedit panel 本体 (Phase 22 では sample 側にビルドさせる、Module.cmake 不変)。
    ${CMAKE_SOURCE_DIR}/src/gameframework/tools/btedit/BehaviorTreeEditorPanel.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    BtEditorApp.h
    BtEditorScene.h
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
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
