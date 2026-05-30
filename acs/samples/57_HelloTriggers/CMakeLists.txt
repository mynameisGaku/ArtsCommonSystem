# HelloTriggers - Package B 検証コンソール: 衝突レイヤ/マスク + トリガ enter/exit。
add_executable(hello_triggers main.cpp)
acs_apply_compiler_options(hello_triggers)
target_link_libraries(hello_triggers PRIVATE ACS::GameFramework)
