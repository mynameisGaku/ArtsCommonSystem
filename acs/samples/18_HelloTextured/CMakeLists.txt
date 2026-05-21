# 「HelloTextured」サンプル: テクスチャ + サンプラ + 3D 描画
add_executable(hello_textured WIN32 main.cpp)
acs_apply_compiler_options(hello_textured)
target_link_libraries(hello_textured PRIVATE ACS::Game)
