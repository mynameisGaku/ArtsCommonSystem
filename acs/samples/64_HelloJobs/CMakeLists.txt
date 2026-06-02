# 「Hello Jobs」サンプル: acs::easy のジョブ/並列 API 入門。
# easy レイヤは int main() で書くため WIN32 を付けずコンソールサブシステムでビルド。
# ジョブ結果をコンソールに出力して自己検証する (=== ALL PASS ===)。
add_executable(hello_jobs
    main.cpp
)
acs_apply_compiler_options(hello_jobs)
target_include_directories(hello_jobs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_jobs PRIVATE ACS::Easy)
