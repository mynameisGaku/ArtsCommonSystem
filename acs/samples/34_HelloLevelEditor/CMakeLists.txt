# HelloLevelEditor — GameFramework Phase 22: Tilemap (Level) Editor
# editor_core (Phase 21a) + leveledit/ (Phase 22) + Tilemap (Pillar Q) を
# 1 個の ImGui Workspace に統合した level editor デモ。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# / 31_HelloModelViewer / 32_HelloAnimCurveEditor と同じ。
add_executable(hello_level_editor WIN32 main.cpp)
acs_apply_compiler_options(hello_level_editor)
target_link_libraries(hello_level_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui      # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
    ACS::Math
)
