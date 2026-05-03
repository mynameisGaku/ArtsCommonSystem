# 「Hello ImGui」サンプル: ImGui ウィンドウを表示する例
add_executable(hello_imgui WIN32 main.cpp)
acs_apply_compiler_options(hello_imgui)
target_link_libraries(hello_imgui PRIVATE
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
    ACS::Imgui
)
