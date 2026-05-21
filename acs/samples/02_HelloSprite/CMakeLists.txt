# 「HelloSprite」サンプル: 2D スプライト + 矩形 + α ブレンド
add_executable(hello_sprite WIN32 main.cpp)
acs_apply_compiler_options(hello_sprite)
target_link_libraries(hello_sprite PRIVATE ACS::Game)
