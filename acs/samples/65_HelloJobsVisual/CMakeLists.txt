# 「Hello Jobs Visual」: ジョブ/並列を「見える化」する easy サンプル(ウィンドウ表示)。
# easy レイヤは int main() で書くため WIN32 を付けない(別窓のコンソールにログが出る)。
add_executable(hello_jobs_visual
    main.cpp
)
acs_apply_compiler_options(hello_jobs_visual)
target_include_directories(hello_jobs_visual PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_jobs_visual PRIVATE ACS::Easy)
