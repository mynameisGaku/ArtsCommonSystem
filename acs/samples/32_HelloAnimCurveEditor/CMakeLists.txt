# HelloAnimCurveEditor — AnimationCurve エディタ。
# editor_core (EditorWorkspace + EditorTheme) と animcurve/AnimCurveEditorPanel
# を 1 つの ImGui Workspace に統合し、AnimationCurve のキー打ち + tangent 編集
# を対話的に行えるサンプル。
#
# 構成:
#   main.cpp               - ACS_GAME_MAIN(AnimCurveApp) のみ
#   AnimCurveApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
#   AnimCurveScene.{h,cpp} - Workspace + Panel + Curve を持つ Scene
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# / 31_HelloModelViewer と同じ。
add_executable(hello_anim_curve_editor WIN32
    main.cpp
    AnimCurveApp.cpp
    AnimCurveScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    AnimCurveApp.h
    AnimCurveScene.h
)
acs_apply_compiler_options(hello_anim_curve_editor)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "AnimCurveApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_anim_curve_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_anim_curve_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui      # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
    ACS::Math
)
