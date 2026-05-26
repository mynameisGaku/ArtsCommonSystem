# 「Hello Window」サンプル: ウィンドウを開いて背景色で塗るだけの最小例
#
# 構成:
#   main.cpp                - ACS_DEFINE_MAIN(HelloWindowApp) のみ
#   HelloWindowApp.{h,cpp}  - Application 派生クラス本体
add_executable(hello_window WIN32
    main.cpp
    HelloWindowApp.cpp
    HelloWindowApp.h
)
acs_apply_compiler_options(hello_window)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloWindowApp.h" のように local include で書けるようにする)
target_include_directories(hello_window PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_window PRIVATE ACS::Game)

# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
