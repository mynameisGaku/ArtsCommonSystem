# Hello Audio: ファイルから音を読んで再生する例
add_executable(hello_audio WIN32 main.cpp)
acs_apply_compiler_options(hello_audio)
target_link_libraries(hello_audio PRIVATE
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
    ACS::Audio
)
