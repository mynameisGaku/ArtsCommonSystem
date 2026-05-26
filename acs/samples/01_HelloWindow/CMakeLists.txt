# HelloWindow: ウィンドウを開いて背景色で塗るだけの最小サンプル。
add_executable(hello_window WIN32
    main.cpp
    HelloWindowApp.cpp
    HelloWindowApp.h
)
acs_apply_compiler_options(hello_window)
# 同一サンプル内の .h を "HelloWindowApp.h" のように local include できるように、
# サンプル自身の dir を PRIVATE include path に追加する。
target_include_directories(hello_window PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_window PRIVATE ACS::Game)

# Visual Studio の Solution Explorer で .cpp / .h をフォルダ分けする。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
