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
add_executable(hello_cinematics_editor WIN32
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.cpp
)
acs_apply_compiler_options(hello_cinematics_editor)
target_include_directories(hello_cinematics_editor PRIVATE
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
