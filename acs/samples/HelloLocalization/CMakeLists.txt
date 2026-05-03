# 「HelloLocalization」サンプル: 多言語対応 (i18n)
add_executable(hello_localization WIN32 main.cpp)
acs_apply_compiler_options(hello_localization)
target_link_libraries(hello_localization PRIVATE
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
