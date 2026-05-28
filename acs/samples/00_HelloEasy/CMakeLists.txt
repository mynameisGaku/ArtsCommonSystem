# 「Hello Easy」サンプル: acs::easy レイヤを使う最も簡単な入門
#
# easy レイヤは（WinMain ではなく）ふつうの int main() で書くため、
# WIN32 を付けずコンソールサブシステムでビルドする。学習中はログが
# コンソールに表示されるのでむしろ都合がよい。
add_executable(hello_easy
    main.cpp
    HelloEasyApp.cpp
    HelloEasyApp.h
)
acs_apply_compiler_options(hello_easy)
# 同一サンプル内の .h を "HelloEasyApp.h" のように local include で書けるようにする
target_include_directories(hello_easy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_easy PRIVATE ACS::Easy)

# 配布 ZIP 化の reference 利用例。`cmake --build <build> --target package` で
# hello_easy.exe + 依存 DLL + (assets/) を含む ZIP が build/ 直下に生成される。
# 利用者の自作ゲーム CMakeLists でも同じ 1 行で OK。
acs_package_game(hello_easy)
