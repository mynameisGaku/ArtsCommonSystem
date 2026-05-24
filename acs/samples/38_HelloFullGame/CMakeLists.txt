# HelloFullGame — GameFramework Pillar A-X を実際に使う dogfood サンプル。
# Title → Gameplay → GameOver の 3 シーン構成、5 wave の top-down shoot-em-up。
# 5 分くらいで遊べる完結ミニゲーム。
add_executable(hello_full_game WIN32 main.cpp)
acs_apply_compiler_options(hello_full_game)
target_link_libraries(hello_full_game PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::App
    ACS::Platform
    ACS::Imgui
    ACS::Math
    ACS::Memory
    ACS::Foundation
    ACS::Container
)
