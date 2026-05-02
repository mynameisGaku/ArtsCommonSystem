# 「Hello Triangle」サンプル: 3 角形を描画する最小例
add_executable(hello_triangle WIN32 main.cpp)
acs_apply_compiler_options(hello_triangle)
target_link_libraries(hello_triangle PRIVATE
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
