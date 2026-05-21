# 「Hello Triangle」サンプル: 3 角形を描画する最小例
add_executable(hello_triangle WIN32 main.cpp)
acs_apply_compiler_options(hello_triangle)
target_link_libraries(hello_triangle PRIVATE ACS::Game)
