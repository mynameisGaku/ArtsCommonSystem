# 「HelloModel」サンプル: 標準ライティング + 手続きプリミティブ + 非同期ロード
add_executable(hello_model WIN32 main.cpp)
acs_apply_compiler_options(hello_model)
target_link_libraries(hello_model PRIVATE ACS::Game)
