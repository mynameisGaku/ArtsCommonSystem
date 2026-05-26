# HelloModelViewer — GameFramework Phase 21b: 3D Model Viewer エディタ
# editor_core (Phase 21a) + modelview/ (Phase 21b) + sample 17 同等の PBR
# 3D pipeline を 1 つの ImGui Workspace に統合した model viewer デモ。
#
# 構成:
#   main.cpp                - ACS_GAME_MAIN(ModelViewerApp) のみ
#   ModelViewerApp.{h,cpp}  - Game 派生クラス (ImGui lifecycle ラッパ)
#   ModelViewerScene.{h,cpp}- Workspace + 4 panel + AssetBrowser + Theme + 3D cube
#   ViewerCubeAssets.h      - cube 頂点 / インデックス / HLSL (constexpr 定数)
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui / 29_HelloParticleEditor / 30_HelloSceneInspector
# と同じ。
add_executable(hello_model_viewer WIN32
    main.cpp
    ModelViewerApp.cpp
    ModelViewerScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    ModelViewerApp.h
    ModelViewerScene.h
    ViewerCubeAssets.h
)
acs_apply_compiler_options(hello_model_viewer)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "ModelViewerApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_model_viewer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_model_viewer PRIVATE
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
