# 「HelloShadows」サンプル: シャドウマッピング (Vogel PCSS 16+24)
#
# 構成:
#   main.cpp                  - ACS_DEFINE_MAIN(HelloShadowsApp) のみ
#   Types.h                   - CasterInst POD + 共通定数
#   ShadowsScene.{h,cpp}      - シーン描画ロジック (シャドウパス + 主パス)
#   HelloShadowsApp.{h,cpp}   - Application 派生クラス
add_executable(hello_shadows WIN32
    main.cpp
    ShadowsScene.cpp
    HelloShadowsApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    ShadowsScene.h
    HelloShadowsApp.h
)
acs_apply_compiler_options(hello_shadows)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloShadowsApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_shadows PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_shadows PRIVATE ACS::Game)
