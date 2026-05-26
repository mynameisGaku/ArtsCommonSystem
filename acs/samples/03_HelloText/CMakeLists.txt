# 「HelloText」サンプル: TTF フォント + UTF-8 テキスト描画
#
# 構成:
#   main.cpp                - ACS_DEFINE_MAIN(HelloTextApp) のみ
#   HelloTextApp.{h,cpp}    - Application 派生クラス本体
add_executable(hello_text WIN32
    main.cpp
    HelloTextApp.cpp
    HelloTextApp.h
)
acs_apply_compiler_options(hello_text)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloTextApp.h" のように local include で書けるようにする)
target_include_directories(hello_text PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_text PRIVATE ACS::Game)

# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
