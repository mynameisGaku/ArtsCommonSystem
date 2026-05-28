# 「Hello PBR」サンプル: PbrShader (Cook-Torrance BRDF) で material ball
# グリッドを描画する例
#
# 構成:
#   main.cpp            - ACS_DEFINE_MAIN(HelloPbrApp) のみ
#   HelloPbrApp.{h,cpp} - Application 派生クラス (PbrShader + sphere grid + 床)
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
