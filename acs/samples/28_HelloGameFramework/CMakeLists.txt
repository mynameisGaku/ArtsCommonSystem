# HelloGameFramework — GameFramework Pillar A (Phase 1) のデモ。
# 2 シーン切替を見せるだけの最小サンプル。
add_executable(hello_game_framework WIN32 main.cpp)
acs_apply_compiler_options(hello_game_framework)
target_link_libraries(hello_game_framework PRIVATE
    ACS::GameFramework
    ACS::Render
    ACS::Platform
    ACS::App
)
