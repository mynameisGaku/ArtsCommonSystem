# 「HelloLights」サンプル: 点光源 4 灯 + 微弱 dir ライトの暗いシーン
add_executable(hello_lights WIN32 main.cpp)
acs_apply_compiler_options(hello_lights)
target_link_libraries(hello_lights PRIVATE
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
