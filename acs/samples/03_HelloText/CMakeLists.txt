# 「HelloText」サンプル: TTF フォント + UTF-8 テキスト描画
add_executable(hello_text WIN32 main.cpp)
acs_apply_compiler_options(hello_text)
target_link_libraries(hello_text PRIVATE ACS::Game)
