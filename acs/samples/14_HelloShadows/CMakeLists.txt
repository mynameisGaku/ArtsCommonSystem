# 「HelloShadows」サンプル: シャドウマッピング (PCF 4-tap)
add_executable(hello_shadows WIN32 main.cpp)
acs_apply_compiler_options(hello_shadows)
target_link_libraries(hello_shadows PRIVATE ACS::Game)
