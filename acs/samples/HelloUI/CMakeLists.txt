add_executable(hello_ui WIN32 main.cpp)
acs_apply_compiler_options(hello_ui)
target_link_libraries(hello_ui PRIVATE
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
    ACS::Mvvm
    ACS::Ui
    ACS::App
)
