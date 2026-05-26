# 「Hello IBL」サンプル: Phase 31 — Image-Based Lighting + Phase 32a HDR / ACES tonemap
# フル機能 IBL demo (Sky preset / Studio HDR / Atmosphere / SH9 / clear-coat /
# anisotropic / area light / probe grid / fog / shadow CSM / SSR / SSAO / SSGI /
# TAA / motion vector / lightmap / refraction)。
#
# 構成 (1199 行 → 5 ファイルに分割):
#   main.cpp                    - ACS_DEFINE_MAIN(HelloIblApp) のみ
#   IblTypes.h                  - 共通定数 + Halton(2,3) helper
#   IblEnvBuilder.{h,cpp}       - CPU equirect 生成 (Sky procedural / Studio HDR)
#   IblLightmapBaker.{h,cpp}    - 床用 lightmap CPU baker
#   HelloIblApp.{h,cpp}         - Application 派生 (全 pass + HUD)
add_executable(hello_ibl WIN32
    main.cpp
    IblEnvBuilder.cpp
    IblLightmapBaker.cpp
    HelloIblApp.cpp
    IblTypes.h
    IblEnvBuilder.h
    IblLightmapBaker.h
    HelloIblApp.h
)
acs_apply_compiler_options(hello_ibl)
target_include_directories(hello_ibl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_ibl PRIVATE ACS::Game)
source_group("Source Files" REGULAR_EXPRESSION "\\.(cpp|cxx|cc|c)$")
source_group("Header Files" REGULAR_EXPRESSION "\\.(h|hpp|hxx)$")
