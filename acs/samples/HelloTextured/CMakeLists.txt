# 「HelloTextured」サンプル: テクスチャ + サンプラ + 3D 描画
add_executable(hello_textured WIN32 main.cpp)
acs_apply_compiler_options(hello_textured)
target_link_libraries(hello_textured PRIVATE
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
