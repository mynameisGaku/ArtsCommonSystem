# 「HelloSprite」サンプル: 2D スプライト + 矩形 + α ブレンド
#
# 構成:
#   main.cpp                 - ACS_DEFINE_MAIN(HelloSpriteApp) のみ
#   Types.h                  - 共通定数 + Sprite struct + テクスチャ生成
#   HelloSpriteApp.{h,cpp}   - Application 派生クラス本体
add_executable(hello_sprite WIN32
    main.cpp
    HelloSpriteApp.cpp
    HelloSpriteApp.h
    Types.h
)
acs_apply_compiler_options(hello_sprite)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloSpriteApp.h" のように local include で書けるようにする)
target_include_directories(hello_sprite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_sprite PRIVATE ACS::Game)

# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
