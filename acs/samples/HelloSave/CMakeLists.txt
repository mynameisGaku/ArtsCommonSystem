# 「HelloSave」サンプル: Storage で設定 / セーブデータの永続化
add_executable(hello_save WIN32 main.cpp)
acs_apply_compiler_options(hello_save)
target_link_libraries(hello_save PRIVATE
    ACS::Foundation
    ACS::Threading
    ACS::Memory
    ACS::Container
    ACS::Math
    ACS::Platform
    ACS::Asset
    ACS::Ecs
    ACS::Render
    ACS::App
)
