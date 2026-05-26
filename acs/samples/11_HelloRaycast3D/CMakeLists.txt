# 「HelloRaycast3D」サンプル: 3D 衝突 + マルチライト + Blinn-Phong
#
# 構成:
#   main.cpp                  - ACS_DEFINE_MAIN(HelloRaycast3DApp) のみ
#   Types.h                   - Object POD (Sphere / Cube) + 配置 / カメラ定数
#   HelloRaycast3DApp.{h,cpp} - Application 派生クラス
#   RaycastScene.{h,cpp}      - メッシュ + レイキャスト + マルチライト描画 + HUD
add_executable(hello_raycast3d WIN32
    main.cpp
    HelloRaycast3DApp.cpp
    RaycastScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    HelloRaycast3DApp.h
    RaycastScene.h
)
acs_apply_compiler_options(hello_raycast3d)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "Types.h"
# のように local include で書けるようにする)
target_include_directories(hello_raycast3d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_raycast3d PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
