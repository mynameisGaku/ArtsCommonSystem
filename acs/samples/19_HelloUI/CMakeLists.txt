add_executable(hello_ui WIN32 main.cpp)
acs_apply_compiler_options(hello_ui)
target_link_libraries(hello_ui PRIVATE
    ACS::Game
    ACS::Event   # 標準セットに含まれない追加モジュール
    ACS::Mvvm
    ACS::Ui
)
