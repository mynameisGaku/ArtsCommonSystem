# HelloSpriteAtlasEditor — SpriteAtlas エディタのデモ。
# editor_core ワークスペースに FSpriteAtlasEditorPanel を載せ、
# 256x256 dummy atlas + 3 frame (Idle / Walk / Jump) を ImGui 上で対話編集する。
#
# 必須バックエンド: ACS_RENDER_DX12_RAW (ImGui 統合は DX12 raw backend 専用)。
#
# 構成:
#   main.cpp                 - ACS_GAME_MAIN(SpriteAtlasApp) のみ
#   SpriteAtlasApp.{h,cpp}   - FGame 派生クラス (ImGui lifecycle ラッパ)
#   SpriteAtlasScene.{h,cpp} - Workspace + FSpriteAtlasEditorPanel + dummy FSpritePack
add_executable(hello_sprite_atlas_editor WIN32
    main.cpp
    SpriteAtlasApp.cpp
    SpriteAtlasScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    SpriteAtlasApp.h
    SpriteAtlasScene.h
)
acs_apply_compiler_options(hello_sprite_atlas_editor)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "SpriteAtlasApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_sprite_atlas_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_sprite_atlas_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
    ACS::Imgui   # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
