# HelloFullGame — GameFramework Pillar A-X を実際に使う dogfood サンプル。
# Title → Gameplay → GameOver の 3 シーン構成、5 wave の top-down shoot-em-up。
# 5 分くらいで遊べる完結ミニゲーム。
#
# 構成:
#   main.cpp           - ACS_GAME_MAIN(FullGameApp) のみ
#   GameTypes.h        - HighScore POD + 共通定数
#   FullGameApp.{h,cpp}- FGame 派生クラス
#   TitleScene.{h,cpp} - Title 画面
#   GameplayScene.{h,cpp} - 本編 (シーン skeleton + module orchestration)
#   GameOverScene.{h,cpp} - 結果画面
#
# Gameplay scene は機能別 module に分割されている (各 <200 LOC):
#   Player.{h,cpp}        - player node / 入力 / 移動 / 発射クールダウン
#   Enemy.{h,cpp}         - 敵 pool / spawn / AI 追跡 / 接触判定 / 描画
#   Bullets.{h,cpp}       - FProjectileSystem ラッパ / 発射 / 軌跡描画
#   HitEffects.{h,cpp}    - ParticleEffect + FEffectSystem (shake / flash)
#   Hud.{h,cpp}           - HP / score / wave / FPS HUD 描画
#   WaveCallbacks.{h,cpp} - FWaveSpawner / FProjectileSystem の C 関数ブリッジ
add_executable(hello_full_game WIN32
    main.cpp
    FullGameApp.cpp
    TitleScene.cpp
    GameplayScene.cpp
    GameOverScene.cpp
    Player.cpp
    Enemy.cpp
    Bullets.cpp
    HitEffects.cpp
    Hud.cpp
    WaveCallbacks.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    GameTypes.h
    FullGameApp.h
    TitleScene.h
    GameplayScene.h
    GameOverScene.h
    Player.h
    Enemy.h
    Bullets.h
    HitEffects.h
    Hud.h
    WaveCallbacks.h
)
acs_apply_compiler_options(hello_full_game)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "FullGameApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_full_game PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_full_game PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui
    ACS::Math
    ACS::Memory
    ACS::Foundation
    ACS::Container
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
