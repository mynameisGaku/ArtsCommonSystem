# HelloCinematicsEditor — GameFramework Phase 23: Cinematics Timeline Editor
# editor_core (Phase 21a) + cinetimeline/CinematicsTimelineEditorPanel (Phase 23)
# + CinematicsDirector (Pillar R Phase 2) を 1 つの ImGui Workspace に統合した、
# タイムラインキーフレーム編集 + 再生制御を対話的に行えるサンプル。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# / 31_HelloModelViewer / 32_HelloAnimCurveEditor と同じ。
#
# 設計メモ:
#   ・CinematicsTimelineEditorPanel の .cpp は **本 sample の executable に直接
#     コンパイル** する。理由は Phase 23 では GameFramework Module.cmake を
#     編集しない方針 (= sample 側で完結させる) のため。将来 panel が他 sample
#     からも参照されるようになったら Module.cmake に編入する。
#   ・src/gameframework/tools/cinetimeline/ を target_sources で混ぜ込み、
#     include path は ACS_SOURCE_ROOT (= src/) を target_include_directories で
#     渡す (= ACS::GameFramework が既に同 path を PUBLIC に持つので link でも
#     伝播するが、念のため明示)。
#
# 構成 (sample 38 と同パターンに分割):
#   main.cpp                - ACS_GAME_MAIN(CineEditorApp) のみ
#   CineEditorApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
#   CineEditorScene.{h,cpp} - Workspace + CinematicsTimelineEditorPanel +
#                             CinematicsDirector + 初期 keyframe 3 個
add_executable(hello_cinematics_editor WIN32
    main.cpp
    CineEditorApp.cpp
    CineEditorScene.cpp
    ${CMAKE_SOURCE_DIR}/src/gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    CineEditorApp.h
    CineEditorScene.h
)
acs_apply_compiler_options(hello_cinematics_editor)
target_include_directories(hello_cinematics_editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(hello_cinematics_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui      # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
    ACS::Math
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
