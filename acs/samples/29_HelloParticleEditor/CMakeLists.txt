# SPDX-License-Identifier: Apache-2.0
# HelloParticleEditor — GameFramework Phase 19b: ParticleEditor デモ。
# ParticleEffectSystem (Phase 14) を `fxedit::ParticleEditorPanel` /
# `fxedit::ParticleEditorPreview` で対話編集し、`.fxedit` 形式に Save/Load する
# 単一ウィンドウのエディタサンプル。
#
# 必須バックエンド: ImGui 統合は ACS_RENDER_DX12_RAW 専用 (acs_imgui module の
# 制約)。samples/21_HelloImGui と同じ。
#
# 構成:
#   main.cpp                    - ACS_GAME_MAIN(ParticleEditorApp) のみ
#   ParticleEditorScene.{h,cpp} - emitter + Panel + Preview を持つ Scene
#   ParticleEditorApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
add_executable(hello_particle_editor WIN32
    main.cpp
    ParticleEditorScene.cpp
    ParticleEditorApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    ParticleEditorScene.h
    ParticleEditorApp.h
)
acs_apply_compiler_options(hello_particle_editor)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "ParticleEditorApp.h" のように local include で書けるようにする)
target_include_directories(hello_particle_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_particle_editor PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
    ACS::Imgui   # Dear ImGui 統合 (標準セットに含まれない追加モジュール)
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
