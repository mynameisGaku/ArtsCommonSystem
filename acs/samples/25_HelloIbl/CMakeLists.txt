# 「Hello IBL」サンプル: Image-Based Lighting + HDR / ACES tonemap
# フル機能 IBL demo (FSky preset / Studio HDR / FAtmosphere / SH9 / clear-coat /
# anisotropic / area light / probe grid / fog / shadow CSM / SSR / SSAO / SSGI /
# TAA / motion vector / lightmap / refraction)。
#
# 機能ごとに pass helper を分けて、HelloIblApp.cpp は orchestration のみに痩せる。
add_executable(hello_ibl WIN32
    main.cpp
    HelloIblApp.cpp
    IblEnvBuilder.cpp
    IblLightmapBaker.cpp
    IblPresetBuilder.cpp
    ShadowPass.cpp
    GBufferPass.cpp
    ScreenSpaceEffects.cpp
    RefractionPass.cpp
    DynamicOrbs.cpp
    SceneDraw.cpp
    PbrLightingBindings.cpp
    TaaJitter.cpp
    ExposureControl.cpp
    HudOverlay.cpp
    HelloIblApp.h
    IblTypes.h
    IblEnvBuilder.h
    IblLightmapBaker.h
)
acs_apply_compiler_options(hello_ibl)
target_include_directories(hello_ibl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_ibl PRIVATE ACS::Game)
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
