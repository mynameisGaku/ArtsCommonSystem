# 「HelloPhysics2D」サンプル: 2D 円衝突 + 重力 + 反射
#
# 構成:
#   main.cpp                  - ACS_DEFINE_MAIN(HelloPhysics2DApp) のみ
#   Types.h                   - Ball POD + 物理 / 描画パラメータ定数
#   HelloPhysics2DApp.{h,cpp} - Application 派生クラス
#   PhysicsScene.{h,cpp}      - 物理シミュレーション + 描画
add_executable(hello_physics2d WIN32
    main.cpp
    HelloPhysics2DApp.cpp
    PhysicsScene.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    HelloPhysics2DApp.h
    PhysicsScene.h
)
acs_apply_compiler_options(hello_physics2d)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "Types.h"
# のように local include で書けるようにする)
target_include_directories(hello_physics2d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_physics2d PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
