add_executable(hello_mvvm WIN32 main.cpp)
acs_apply_compiler_options(hello_mvvm)
target_link_libraries(hello_mvvm PRIVATE
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
    ACS::Imgui
    ACS::Mvvm
)
