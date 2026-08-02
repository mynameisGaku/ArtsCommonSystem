# HelloLight2D - 2D 動的ライト + ソフト影 (Core Keeper 風) + ブロブ影デモ。
#
# 構成:
#   main.cpp            - ACS_DEFINE_MAIN(CLight2DApp)
#   Light2DApp.{h,cpp}  - CApplication 派生 (OnCustomFrame で scene/occluder/composite)
#
# DX12 raw で動く 2D サンプル。CLighting2D / CBlobShadow は ACS::Render にある。
add_executable(hello_light2d WIN32
    main.cpp
    Light2DApp.cpp
    Light2DApp.h
)
acs_apply_compiler_options(hello_light2d)
target_include_directories(hello_light2d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(hello_light2d PRIVATE ACS::Game)
