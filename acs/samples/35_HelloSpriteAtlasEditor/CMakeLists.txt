# HelloSpriteAtlasEditor — GameFramework Phase 22: SpriteAtlasEditor デモ
# editor_core (Phase 21a) + spriteatlas/ (Phase 22) を 1 つの ImGui Workspace に
# 統合し、SpritePack (256x256 dummy atlas + 3 frame) を対話編集する単一
# ウィンドウのエディタサンプル。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# / 31_HelloModelViewer と同じ。
add_executable(hello_sprite_atlas_editor WIN32 main.cpp)
acs_apply_compiler_options(hello_sprite_atlas_editor)
target_link_libraries(hello_sprite_atlas_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
    ACS::Imgui   # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
)
