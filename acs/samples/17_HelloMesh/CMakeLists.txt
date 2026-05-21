# 「HelloMesh」サンプル: 3D 回転キューブ + 深度テスト + 定数バッファ
add_executable(hello_mesh WIN32 main.cpp)
acs_apply_compiler_options(hello_mesh)
target_link_libraries(hello_mesh PRIVATE ACS::Game)
