# 「HelloLights」サンプル: 点光源 4 灯 + 微弱 dir ライトの暗いシーン
#
# 構成:
#   main.cpp                 - ACS_DEFINE_MAIN(HelloLightsApp) のみ
#   Types.h                  - ObjectInst POD + 共通定数
#   LightsScene.{h,cpp}      - シーン描画ロジック
#   HelloLightsApp.{h,cpp}   - Application 派生クラス
add_executable(hello_lights WIN32
    main.cpp
    LightsScene.cpp
    HelloLightsApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    LightsScene.h
    HelloLightsApp.h
)
acs_apply_compiler_options(hello_lights)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloLightsApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_lights PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_lights PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
