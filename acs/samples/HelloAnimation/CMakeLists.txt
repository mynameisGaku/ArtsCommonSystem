# 「HelloAnimation」サンプル: スキンメッシュ + GPU スキニング
add_executable(hello_animation WIN32 main.cpp)
acs_apply_compiler_options(hello_animation)
target_link_libraries(hello_animation PRIVATE
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
