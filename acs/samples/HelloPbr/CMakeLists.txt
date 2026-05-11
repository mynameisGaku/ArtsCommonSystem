# 「Hello PBR」サンプル: PbrShader (Cook-Torrance BRDF) で material ball
# グリッドを描画する例
add_executable(hello_pbr WIN32 main.cpp)
acs_apply_compiler_options(hello_pbr)
target_link_libraries(hello_pbr PRIVATE
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
