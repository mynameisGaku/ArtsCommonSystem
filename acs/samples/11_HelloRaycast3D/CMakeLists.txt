# 「HelloRaycast3D」サンプル: 3D 衝突 + マルチライト + Blinn-Phong
#
# ファイル構成:
#   main.cpp                  - ACS_DEFINE_MAIN(HelloRaycast3DApp) のみ
#   Types.h                   - Object POD + 配置 / カメラ定数
#   HelloRaycast3DApp.{h,cpp} - Application 派生 (リソース所有)
#   RaycastScene.{h,cpp}      - オーケストレーション (メッシュ + 描画)
#   RaycastTargets.{h,cpp}    - オブジェクト配列 + ヒット状態
#   RayCaster.{h,cpp}         - カメラ入力 + レイ判定
#   HudRenderer.{h,cpp}       - 2D HUD (クロスヘア + 当たり情報)
add_executable(hello_raycast3d WIN32
    main.cpp
    HelloRaycast3DApp.cpp
    RaycastScene.cpp
    RaycastTargets.cpp
    RayCaster.cpp
    HudRenderer.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    HelloRaycast3DApp.h
    RaycastScene.h
    RaycastTargets.h
    RayCaster.h
    HudRenderer.h
)
acs_apply_compiler_options(hello_raycast3d)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "Types.h"
# のように local include で書けるようにする)
target_include_directories(hello_raycast3d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_raycast3d PRIVATE ACS::Game)
