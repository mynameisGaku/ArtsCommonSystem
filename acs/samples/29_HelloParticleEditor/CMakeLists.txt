# HelloParticleEditor — GameFramework Phase 19b: ParticleEditor デモ。
# ParticleEffectSystem (Phase 14) を `fxedit::ParticleEditorPanel` /
# `fxedit::ParticleEditorPreview` で対話編集し、`.fxedit` 形式に Save/Load する
# 単一ウィンドウのエディタサンプル。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui と同じ。
add_executable(hello_particle_editor WIN32 main.cpp)
acs_apply_compiler_options(hello_particle_editor)
target_link_libraries(hello_particle_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
    ACS::Imgui   # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
)
