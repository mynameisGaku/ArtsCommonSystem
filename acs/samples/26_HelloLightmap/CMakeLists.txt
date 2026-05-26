# 「Hello Lightmap」サンプル: Phase 33f-3 — multi-bounce path-traced lightmap
# Cornell box シーンを CPU で multi-bounce ベイクし、PbrShader の lightmap slot
# (Phase 33f で追加した t8) 経由で表示する。
#
# 構成:
#   main.cpp                 - ACS_DEFINE_MAIN(HelloLightmapApp) のみ
#   LightmapTypes.h          - Quad / Rng / 数学ヘルパ / constexpr 定数
#   LightmapBaker.{h,cpp}    - CPU multi-bounce path tracing
#   HelloLightmapApp.{h,cpp} - Application 派生 (Cornell box 構築 + OnCustomFrame)
add_executable(hello_lightmap WIN32
    main.cpp
    LightmapBaker.cpp
    HelloLightmapApp.cpp
    LightmapTypes.h
    LightmapBaker.h
    HelloLightmapApp.h
)
acs_apply_compiler_options(hello_lightmap)
target_include_directories(hello_lightmap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_lightmap PRIVATE ACS::Game)
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
