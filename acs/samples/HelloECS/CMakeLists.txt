add_executable(hello_ecs WIN32 main.cpp)
acs_apply_compiler_options(hello_ecs)
target_link_libraries(hello_ecs PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Platform
    ACS::Asset
    ACS::Ecs
    ACS::Event
    ACS::Render
    ACS::App
)
