# HelloSceneInspector — GameFramework Phase 20 editor 第二弾: SceneInspector デモ。
# Node2D ツリー (root → wheel → spoke[0/1] のような複数階層) を
# `inspector::HierarchyPanel` で可視化し、`inspector::InspectorPanel` が
# `IInspectableProvider` 経由で公開された field を ImGui で編集する単一
# ウィンドウのエディタサンプル。`inspector::EditorToolbar` で Play/Pause/Step
# などのコマンドを発行、`inspector::SelectionService` で 4 パネル間の選択を共有。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor と同じ。
add_executable(hello_scene_inspector WIN32 main.cpp)
acs_apply_compiler_options(hello_scene_inspector)
target_link_libraries(hello_scene_inspector PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
    ACS::Imgui   # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
)
