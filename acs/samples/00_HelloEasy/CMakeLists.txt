# 「Hello Easy」サンプル: acs::easy レイヤを使う最も簡単な入門
#
# 注: easy レイヤは（WinMain ではなく）ふつうの int main() で書くため、
#     WIN32 を付けずコンソールサブシステムでビルドする。学習中はログが
#     コンソールに表示されるのでむしろ都合がよい。
add_executable(hello_easy main.cpp)
acs_apply_compiler_options(hello_easy)
target_link_libraries(hello_easy PRIVATE ACS::Easy)
