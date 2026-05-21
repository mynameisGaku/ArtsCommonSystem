add_executable(hello_ecs WIN32 main.cpp)
acs_apply_compiler_options(hello_ecs)
target_link_libraries(hello_ecs PRIVATE
    ACS::Game
    ACS::Event   # TimerManager / MessageBroker（標準セットに含まれない追加モジュール）
)
