# HelloAnimCurveEditor — GameFramework Phase 22: AnimationCurve エディタ
# editor_core (Phase 21a) + animcurve/AnimCurveEditorPanel (Phase 22) を
# 1 つの ImGui Workspace に統合した、AnimationCurve のキー打ち + tangent
# 編集を対話的に行えるサンプル。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# / 31_HelloModelViewer と同じ。
add_executable(hello_anim_curve_editor WIN32 main.cpp)
acs_apply_compiler_options(hello_anim_curve_editor)
target_link_libraries(hello_anim_curve_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui      # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
    ACS::Math
)
