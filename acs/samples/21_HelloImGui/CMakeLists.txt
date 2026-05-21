# 「Hello ImGui」サンプル: ImGui ウィンドウを表示する例
add_executable(hello_imgui WIN32 main.cpp)
acs_apply_compiler_options(hello_imgui)
target_link_libraries(hello_imgui PRIVATE
    ACS::Game
    ACS::Imgui   # Dear ImGui 統合（標準セットに含まれない追加モジュール）
)
