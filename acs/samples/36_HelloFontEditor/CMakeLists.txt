# HelloFontEditor — フォントの fallback chain を対話編集するエディタサンプル。
# editor_core ワークスペースに FFontEditorPanel を統合し、3 face
# (Noto Sans JP / Noto Sans Mono / fallback emoji) を初期登録した状態で起動する。
#
# 必須バックエンド: ACS_RENDER_DX12_RAW (acs_imgui module の制約で、ImGui 統合は
# DX12 raw backend 専用)。
#
# 構成:
#   main.cpp                - ACS_GAME_MAIN(FontEditorApp) のみ
#   FontEditorApp.{h,cpp}   - FGame 派生クラス (ImGui lifecycle ラッパ)
#   FontEditorScene.{h,cpp} - Workspace + FFontEditorPanel + 初期 3 face 登録
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
