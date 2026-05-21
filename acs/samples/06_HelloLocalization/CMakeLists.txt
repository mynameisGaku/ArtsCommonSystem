# 「HelloLocalization」サンプル: 多言語対応 (i18n)
add_executable(hello_localization WIN32 main.cpp)
acs_apply_compiler_options(hello_localization)
target_link_libraries(hello_localization PRIVATE ACS::Game)
