# 「Hello Window」サンプル: ウィンドウを開いて背景色で塗るだけの最小例
add_executable(hello_window WIN32 main.cpp)
acs_apply_compiler_options(hello_window)
target_link_libraries(hello_window PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Platform
    ACS::Asset
    ACS::Ecs
    ACS::Render
    ACS::App
)
