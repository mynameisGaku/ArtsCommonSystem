# HelloFontEditor — GameFramework Phase 23: FontEditor デモ
# editor_core (Phase 21a) + fontedit/ (Phase 23) を 1 つの ImGui Workspace に
# 統合し、3 face (Noto Sans JP / Noto Sans Mono / fallback emoji) を初期登録
# した fallback chain を対話編集する単一ウィンドウのエディタサンプル。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# / 31_HelloModelViewer / 35_HelloSpriteAtlasEditor と同じ。
#
# 構成 (sample 38 と同パターンに分割):
#   main.cpp                - ACS_GAME_MAIN(FontEditorApp) のみ
#   FontEditorApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
#   FontEditorScene.{h,cpp} - Workspace + FontEditorPanel + 初期 3 face 登録
add_executable(hello_font_editor WIN32
    main.cpp
    FontEditorApp.cpp
    FontEditorScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    FontEditorApp.h
    FontEditorScene.h
)
acs_apply_compiler_options(hello_font_editor)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "FontEditorApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_font_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_font_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
    ACS::Imgui   # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
