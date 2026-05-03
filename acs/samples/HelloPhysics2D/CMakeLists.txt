# 「HelloPhysics2D」サンプル: 2D 円衝突 + 重力 + 反射
add_executable(hello_physics2d WIN32 main.cpp)
acs_apply_compiler_options(hello_physics2d)
target_link_libraries(hello_physics2d PRIVATE
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
