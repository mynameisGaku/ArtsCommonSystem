add_executable(hello_mvvm WIN32 main.cpp)
acs_apply_compiler_options(hello_mvvm)
target_link_libraries(hello_mvvm PRIVATE
    ACS::Game
    ACS::Event   # 標準セットに含まれない追加モジュール
    ACS::Imgui
    ACS::Mvvm
)
