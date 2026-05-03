# 「HelloText」サンプル: TTF フォント + UTF-8 テキスト描画
add_executable(hello_text WIN32 main.cpp)
acs_apply_compiler_options(hello_text)
target_link_libraries(hello_text PRIVATE
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
