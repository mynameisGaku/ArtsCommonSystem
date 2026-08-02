# SPDX-License-Identifier: Apache-2.0
# HelloGameFramework — GameFramework Pillar A〜F の dogfood サンプル。
# Title / Gameplay / Pause の 3 シーンで主要機能を網羅。
#
# 構成:
#   main.cpp                  - ACS_GAME_MAIN(HelloGfApp) のみ
#   PlayerProfile.h           - AppState サンプル struct
#   RotateComponent.{h,cpp}   - composition 版の回転 (AComponent)
#   RotatingNode.{h,cpp}      - 継承版の回転 (ANode サブクラス)
#   TitleScene.{h,cpp}        - Title 画面 (FSM Idle/Blink)
#   GameplayScene.{h,cpp}     - 本編 (ANode ツリー + CCamera2D + 2D Physics)
#   PauseScene.{h,cpp}        - 一時停止 overlay (Sequence Loop)
#   HelloGfApp.{h,cpp}        - Game 派生クラス
add_executable(hello_game_framework WIN32
    main.cpp
    HelloGfApp.cpp
    RotateComponent.cpp
    RotatingNode.cpp
    TitleScene.cpp
    GameplayScene.cpp
    PauseScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    PlayerProfile.h
    RotateComponent.h
    RotatingNode.h
    HelloGfApp.h
    TitleScene.h
    GameplayScene.h
    PauseScene.h
)
acs_apply_compiler_options(hello_game_framework)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloGfApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_game_framework PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_game_framework PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
)
