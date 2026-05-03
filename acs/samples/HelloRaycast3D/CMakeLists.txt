# 「HelloRaycast3D」サンプル: 3D 衝突 + マルチライト + Blinn-Phong
add_executable(hello_raycast3d WIN32 main.cpp)
acs_apply_compiler_options(hello_raycast3d)
target_link_libraries(hello_raycast3d PRIVATE
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
