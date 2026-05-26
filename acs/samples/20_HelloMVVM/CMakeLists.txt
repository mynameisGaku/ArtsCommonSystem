# Hello MVVM: Observable / Binder / Derived / Command を ImGui で紹介するサンプル。
add_executable(hello_mvvm WIN32
    main.cpp
    HelloMVVMApp.cpp
    HelloMVVMApp.h
    PlayerVM.h
)
acs_apply_compiler_options(hello_mvvm)
# サンプル自身の dir を include path に追加し、同一サンプル内の .h を
# "HelloMVVMApp.h" のように相対 include で書けるようにする。
target_include_directories(hello_mvvm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_mvvm PRIVATE
    ACS::Game
    ACS::Event   # 標準セットに含まれない追加モジュール
    ACS::Imgui
    ACS::Mvvm
)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
