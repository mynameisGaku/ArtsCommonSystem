# 「HelloSky」サンプル: 手続き生成スカイ + ライティング統合
#
# 構成:
#   main.cpp               - ACS_DEFINE_MAIN(HelloSkyApp) のみ
#   Types.h                - SkyPreset enum + 共通定数
#   SkyScene.{h,cpp}       - シーン描画ロジック
#   HelloSkyApp.{h,cpp}    - Application 派生クラス
add_executable(hello_sky WIN32
    main.cpp
    SkyScene.cpp
    HelloSkyApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    SkyScene.h
    HelloSkyApp.h
)
acs_apply_compiler_options(hello_sky)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloSkyApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_sky PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_sky PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
