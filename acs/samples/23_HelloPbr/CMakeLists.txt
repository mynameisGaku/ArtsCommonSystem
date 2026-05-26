# 「Hello PBR」サンプル: FPbrShader (Cook-Torrance BRDF) で material ball
# グリッドを描画する例
#
# 構成:
#   main.cpp            - ACS_DEFINE_MAIN(HelloPbrApp) のみ
#   HelloPbrApp.{h,cpp} - FApplication 派生クラス (FPbrShader + sphere grid + 床)
add_executable(hello_pbr WIN32
    main.cpp
    HelloPbrApp.cpp
    HelloPbrApp.h
)
acs_apply_compiler_options(hello_pbr)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を "HelloPbrApp.h"
# のように local include で書けるようにする)
target_include_directories(hello_pbr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_pbr PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
