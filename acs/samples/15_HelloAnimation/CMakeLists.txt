# 「HelloAnimation」サンプル: スキンメッシュ + GPU スキニング
#
# 構成:
#   main.cpp                    - ACS_DEFINE_MAIN(HelloAnimationApp) のみ
#   Types.h                     - 共通定数 (kBoneCount / kRingCount ...)
#   AnimationScene.{h,cpp}      - シーン描画ロジック + BuildSnake (生成)
#   HelloAnimationApp.{h,cpp}   - Application 派生クラス
add_executable(hello_animation WIN32
    main.cpp
    AnimationScene.cpp
    HelloAnimationApp.cpp
    # Header 群 (VS Solution Explorer で表示するため)
    Types.h
    AnimationScene.h
    HelloAnimationApp.h
)
acs_apply_compiler_options(hello_animation)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloAnimationApp.h" のように local include で書けるようにする)
target_include_directories(hello_animation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_animation PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
