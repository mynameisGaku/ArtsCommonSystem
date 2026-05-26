# HelloLights サンプル: 点光源 4 灯 + 微弱 dir ライトのシーン。
add_executable(hello_lights WIN32
    main.cpp
    LightsScene.cpp
    HelloLightsApp.cpp
    # ヘッダも target sources に含める: VS の Solution Explorer に出すため。
    Types.h
    LightsScene.h
    HelloLightsApp.h
)
acs_apply_compiler_options(hello_lights)
# 同一サンプル内 .h を "HelloLightsApp.h" のような local include で書けるよう
# サンプル自身の dir を include path に追加。
target_include_directories(hello_lights PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_lights PRIVATE ACS::Game)
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
