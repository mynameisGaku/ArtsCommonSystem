# 「HelloParticles」サンプル: 2D パーティクルエンジン
add_executable(hello_particles WIN32 main.cpp)
acs_apply_compiler_options(hello_particles)
target_link_libraries(hello_particles PRIVATE
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
