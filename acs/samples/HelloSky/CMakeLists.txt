# 「HelloSky」サンプル: 手続き生成スカイ + ライティング統合
add_executable(hello_sky WIN32 main.cpp)
acs_apply_compiler_options(hello_sky)
target_link_libraries(hello_sky PRIVATE
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
