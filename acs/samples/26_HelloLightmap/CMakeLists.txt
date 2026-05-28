# SPDX-License-Identifier: Apache-2.0
# 「Hello Lightmap」サンプル — multi-bounce path-traced lightmap。
# Cornell box シーンを CPU で multi-bounce ベイクし、PbrShader の lightmap slot
# 経由で表示する。
#
# 構成:
#   main.cpp                  - ACS_DEFINE_MAIN(HelloLightmapApp) のみ
#   LightmapTypes.h           - Quad / Rng / 数学ヘルパ / constexpr 定数
#   LightmapBaker.{h,cpp}     - CPU multi-bounce path tracing
#   CornellBox.{h,cpp}        - Cornell box シーンの構築
#   HelloLightmapApp.{h,cpp}  - Application 派生 (OnStart/OnCustomFrame ほか)
add_executable(hello_lightmap WIN32
    main.cpp
    LightmapBaker.cpp
    CornellBox.cpp
    HelloLightmapApp.cpp
    LightmapTypes.h
    LightmapBaker.h
    CornellBox.h
    HelloLightmapApp.h
)
acs_apply_compiler_options(hello_lightmap)
target_include_directories(hello_lightmap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_lightmap PRIVATE ACS::Game)
