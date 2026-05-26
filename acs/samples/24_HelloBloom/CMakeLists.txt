# 「HelloBloom」サンプル: HDR シーン + Bloom + ACES Tonemap
# 注: PostProcess は Diligent backend 専用機能のため、
#     -DACS_RENDER_DILIGENT=ON で configure すること。
#
# 構成:
#   main.cpp            - ACS_DEFINE_MAIN(HelloBloomApp) のみ
#   HelloBloomApp.{h,cpp} - Application 派生 (HDR シーン + Bloom + Tonemap)
add_executable(hello_bloom WIN32
    main.cpp
    HelloBloomApp.cpp
    HelloBloomApp.h
)
acs_apply_compiler_options(hello_bloom)
# sample 自身の dir を include path に追加 (同一サンプル内の .h を
# "HelloBloomApp.h" のように local include で書けるようにする)
target_include_directories(hello_bloom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_bloom PRIVATE ACS::Game)
# Solution Explorer 内で .cpp と .h を Source Files / Header Files にグループ化。
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
