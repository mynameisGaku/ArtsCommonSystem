# 「HelloAnimation」サンプル: スキンメッシュ + GPU スキニング
add_executable(hello_animation WIN32 main.cpp)
acs_apply_compiler_options(hello_animation)
target_link_libraries(hello_animation PRIVATE ACS::Game)
