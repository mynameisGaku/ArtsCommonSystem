# 「HelloBloom」サンプル: HDR シーン + Bloom + ACES Tonemap
# 注: PostProcess は Diligent backend 専用機能のため、
#     -DACS_RENDER_DILIGENT=ON で configure すること。
add_executable(hello_bloom WIN32 main.cpp)
acs_apply_compiler_options(hello_bloom)
target_link_libraries(hello_bloom PRIVATE
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
