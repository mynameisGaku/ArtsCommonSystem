# HelloModelViewer — GameFramework Phase 21b: 3D Model Viewer エディタ
# editor_core (Phase 21a) + modelview/ (Phase 21b) + sample 17 同等の PBR
# 3D pipeline を 1 つの ImGui Workspace に統合した model viewer デモ。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# と同じ。
add_executable(hello_model_viewer WIN32 main.cpp)
acs_apply_compiler_options(hello_model_viewer)
target_link_libraries(hello_model_viewer PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui      # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
    ACS::Math
)
