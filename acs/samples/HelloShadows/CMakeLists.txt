# 「HelloShadows」サンプル: シャドウマッピング (PCF 4-tap)
add_executable(hello_shadows WIN32 main.cpp)
acs_apply_compiler_options(hello_shadows)
target_link_libraries(hello_shadows PRIVATE
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
